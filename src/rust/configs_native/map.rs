//! Scene maps (`*.map`), the model lists of map model packs (`*.pcp/info.cfg`) and skies
//! (`*.sky/info.cfg`).
use super::*;

/// Gives JSON for one optional row through a converter, when the row is present.
fn opt_row<T>(
    row: Option<&Eff>,
    convert: impl FnOnce(&Eff) -> CoreResult<T>,
) -> CoreResult<Option<T>> {
    row.map(convert).transpose()
}

fn vec3(row: &Eff, from: usize) -> CoreResult<Value> {
    floats_json(row, from, 3)
}

fn vec3_slots(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    float_slots(value, key, 3)
}

/// Reads a row whose slots are all optional floats of a fixed count from an optional array field.
fn opt_vec3_slots(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    match value.get(key) {
        Some(_) => vec3_slots(value, key),
        None => Ok(vec![None, None, None]),
    }
}

/// A command, the document key of its rows and the key and read type of each of its slots.
type RowTable = &'static [(&'static str, &'static str, &'static [(&'static str, char)])];

// --- model_list: CMdsList::LoadPCPFile (mdslist.cpp) ---------------------------------------------

const PCP: Pass = Pass {
    name: "pcp_tag",
    commands: &[
        Command {
            name: "MDS",
            layout: "s",
        },
        Command {
            name: "TYPE",
            layout: "i",
        },
        Command {
            name: "FAR_CLIP",
            layout: "f i",
        },
        Command {
            name: "MDS_END",
            layout: "",
        },
    ],
};

fn build_model_list(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let mut models = Vec::new();
    while let Some(mds) = cursor.take("MDS") {
        let model = Obj::new()
            .put("model", text_json(&mds.text(0)?))
            .opt(
                "type",
                opt_row(cursor.take("TYPE"), |r| r.int(0).map(|v| json!(v)))?,
            )
            .opt(
                "far_clip",
                opt_row(cursor.take("FAR_CLIP"), |r| {
                    Ok(json!({"distance": float_json(r.float(0)?), "fade": r.int(1)?}))
                })?,
            );
        cursor.expect("MDS_END")?;
        models.push(model.done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({ "models": models }))
}

fn flatten_model_list(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for model in array(document, "models")? {
        rows.push(Eff::new("MDS", vec![Some(text_field(model, "model")?)]));
        if let Some(kind) = model.get("type") {
            rows.push(Eff::new("TYPE", vec![Some(int_val(kind)?)]));
        }
        if let Some(clip) = model.get("far_clip") {
            rows.push(Eff::new(
                "FAR_CLIP",
                vec![
                    Some(float_field(clip, "distance")?),
                    Some(int_field(clip, "fade")?),
                ],
            ));
        }
        rows.push(Eff::new("MDS_END", vec![]));
    }
    Ok(vec![rows])
}

const MODEL_LIST: Family = Family {
    kind: "model_list",
    passes: &[&PCP],
    // The pack's name is read by no consumer.
    ignored: &["PCP_NAME"],
    build: build_model_list,
    flatten: flatten_model_list,
    ordered: true,
};

// --- sky: CMapSky::LoadPack (mapsky.cpp) ---------------------------------------------------------

const SKY: Pass = Pass {
    name: "mapsky_tag",
    commands: &[
        Command {
            name: "SKY_IMG",
            layout: "i s",
        },
        Command {
            name: "SKY_MDS",
            layout: "i s f",
        },
        Command {
            name: "SKY_ANIME",
            layout: "i s f",
        },
        Command {
            name: "SUN_MDS",
            layout: "i s",
        },
        Command {
            name: "SKYB_MDS",
            layout: "i s f",
        },
        Command {
            name: "SKYB_ANIME",
            layout: "i s f",
        },
        Command {
            name: "SKY_BG",
            layout: "s",
        },
    ],
};

/// Sky commands, the document key of their rows, and their row keys.
const SKY_ROWS: RowTable = &[
    ("SKY_IMG", "images", &[("band", 'i'), ("file", 's')]),
    (
        "SKY_MDS",
        "models",
        &[("band", 'i'), ("file", 's'), ("rotation_degrees", 'f')],
    ),
    (
        "SKY_ANIME",
        "animations",
        &[("band", 'i'), ("frame", 's'), ("speed_degrees", 'f')],
    ),
    ("SUN_MDS", "suns", &[("band", 'i'), ("file", 's')]),
    (
        "SKYB_MDS",
        "back_models",
        &[("band", 'i'), ("file", 's'), ("rotation_degrees", 'f')],
    ),
    (
        "SKYB_ANIME",
        "back_animations",
        &[("band", 'i'), ("frame", 's'), ("speed_degrees", 'f')],
    ),
    ("SKY_BG", "backgrounds", &[("file", 's')]),
];

fn build_sky(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut obj = Obj::new();
    for (command, key, keys) in SKY_ROWS {
        let rows: Vec<Value> = passes[0]
            .iter()
            .filter(|row| row.name == *command)
            .map(|row| {
                let mut entry = Obj::new();
                for (slot, (name, _)) in keys.iter().enumerate() {
                    entry = entry.opt(name, slot_json(row, slot));
                }
                entry.done()
            })
            .collect();
        obj = obj.put(key, Value::Array(rows));
    }
    Ok(obj.done())
}

fn flatten_sky(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for (command, key, keys) in SKY_ROWS {
        for row in array(document, key)? {
            rows.push(row_of(row, command, keys)?);
        }
    }
    Ok(vec![rows])
}

const SKY_FAMILY: Family = Family {
    kind: "sky",
    passes: &[&SKY],
    ignored: &[],
    build: build_sky,
    flatten: flatten_sky,
    // Each command fills its own table; only the order among one command's rows matters.
    ordered: false,
};

// --- scene_map: CMapInfo::LoadMapInfo / AddMapInfo (mapinfo.cpp), CMap::LoadMapFile (mapload.cpp) -

const MAPINFO: Pass = Pass {
    name: "mapinfo_tag",
    commands: &[
        Command {
            name: "IMG",
            layout: "s?",
        },
        Command {
            name: "PCP",
            layout: "s?",
        },
        Command {
            name: "ACTIVE_LIGHT_SET",
            layout: "i",
        },
        Command {
            name: "LIGHT_SET",
            layout: "i",
        },
        Command {
            name: "FOV",
            layout: "",
        },
        Command {
            name: "BGCOLOR",
            layout: "f f f",
        },
        Command {
            name: "BGCOLOR2",
            layout: "f f f",
        },
        Command {
            name: "AMBIENT",
            layout: "f f f",
        },
        // mapLIGHT ignores a light given fewer than four arguments.
        Command {
            name: "LIGHT",
            layout: "i f?3 f?3 f?3 f?6 f?6 f?6",
        },
        Command {
            name: "PLIGHT",
            layout: "i f f f f f f f",
        },
        Command {
            name: "FOG_ENABLE",
            layout: "i",
        },
        Command {
            name: "FOG",
            layout: "f f i?2 i?2 i?2 f?5 f?5",
        },
        Command {
            name: "LIGHT_SET_END",
            layout: "",
        },
        Command {
            name: "FLOOR",
            layout: "f",
        },
        Command {
            name: "CHARA_POS",
            layout: "f f f",
        },
        Command {
            name: "TIME_FLAG",
            layout: "i i f?2 i?3",
        },
        Command {
            name: "TIME_LIGHT_NUM",
            layout: "i",
        },
        Command {
            name: "DEF_FOOT",
            layout: "i",
        },
        Command {
            name: "SKY_INFO",
            layout: "i f f?2",
        },
        Command {
            name: "LENS_FLARE",
            layout: "i",
        },
        Command {
            name: "TIME_CFADE",
            layout: "i",
        },
        Command {
            name: "ALL_SCISSOR",
            layout: "i",
        },
        Command {
            name: "CHARA_LIGHT_ADJUST",
            layout: "i f f f",
        },
    ],
};

const MAP: Pass = Pass {
    name: "map_tag",
    commands: &[
        Command {
            name: "PARTS",
            layout: "s",
        },
        Command {
            name: "FAR_CLIP",
            layout: "f i",
        },
        Command {
            name: "LIGHT_FLAG",
            layout: "i i",
        },
        Command {
            name: "MOVE_FLAG",
            layout: "i i i i",
        },
        Command {
            name: "LOD_START",
            layout: "",
        },
        Command {
            name: "LOD_BLEND",
            layout: "i",
        },
        Command {
            name: "LOD_PIECE",
            layout: "i s",
        },
        Command {
            name: "LOD_END",
            layout: "",
        },
        Command {
            name: "PIECE",
            layout: "s i?",
        },
        Command {
            name: "PIECE_POS",
            layout: "f f f",
        },
        Command {
            name: "PIECE_NAME",
            layout: "s",
        },
        Command {
            name: "PIECE_ROT",
            layout: "f f f",
        },
        Command {
            name: "PIECE_SCALE",
            layout: "f f f",
        },
        Command {
            name: "PIECE_MATERIAL_START",
            layout: "i",
        },
        Command {
            name: "PIECE_MATERIAL",
            layout: "s i f f f f i",
        },
        Command {
            name: "PIECE_MATERIAL_END",
            layout: "-",
        },
        Command {
            name: "PIECE_COL_TYPE",
            layout: "i i?",
        },
        Command {
            name: "PIECE_TIME",
            layout: "f f",
        },
        Command {
            name: "PIECE_END",
            layout: "",
        },
        Command {
            name: "PARTS_END",
            layout: "",
        },
        Command {
            name: "MAP_PARTS",
            layout: "s i?",
        },
        Command {
            name: "MAP_FAR_CLIP",
            layout: "f i",
        },
        Command {
            name: "PARTS_NAME",
            layout: "s",
        },
        Command {
            name: "PARTS_GROUP",
            layout: "s",
        },
        Command {
            name: "PARTS_POS",
            layout: "f f f",
        },
        Command {
            name: "PARTS_ROT",
            layout: "f f f",
        },
        Command {
            name: "PARTS_SCALE",
            layout: "f f f",
        },
        Command {
            name: "MAP_PARTS_END",
            layout: "",
        },
        Command {
            name: "_MAP_INFO_TOP",
            layout: "-",
        },
        Command {
            name: "CAMERA_INFO",
            layout: "i",
        },
        Command {
            name: "FIX_CAMERA",
            layout: "",
        },
        Command {
            name: "FIX_CAMERA_POS",
            layout: "f f f",
        },
        Command {
            name: "FIX_CAMERA_POS2",
            layout: "i f f f",
        },
        Command {
            name: "FIX_CAMERA_OFF_GROUP",
            layout: "i s i",
        },
        Command {
            name: "FIX_CAMERA_RECT",
            layout: "s f f f f f f f?7 f?7 f?7 f?10 f?10 f?10 f?13 f?13 f?13",
        },
        Command {
            name: "FIX_CAMERA_END",
            layout: "",
        },
        Command {
            name: "CAMERA_INFO_END",
            layout: "-",
        },
        // FUNC_POINT reads its count but uses nothing of it.
        Command {
            name: "FUNC_POINT",
            layout: "",
        },
        Command {
            name: "FUNC_DATA",
            layout: "s i",
        },
        Command {
            name: "FUNC_NAME",
            layout: "s",
        },
        Command {
            name: "FUNC_FLAG",
            layout: "i i f f",
        },
        Command {
            name: "FUNC_FIRE_DATA",
            layout: "f f f i i? i?",
        },
        Command {
            name: "FUNC_EFFECT_NAME",
            layout: "s",
        },
        Command {
            name: "FUNC_PLIGHT_DATA",
            layout: "f f f f i i? i? i? i? i? f? f?",
        },
        Command {
            name: "FUNC_ANIME_DATA",
            layout: "s s s i i f f f f f f f f f i? i?",
        },
        Command {
            name: "FUNC_INVENT_DATA",
            layout: "i f f f f f f i f f",
        },
        Command {
            name: "FUNC_EVENT_DATA",
            layout: "s i i i i i s? i? i? i?",
        },
        Command {
            name: "FUNC_POS",
            layout: "f f f f f f f f f",
        },
        Command {
            name: "FUNC_DATA_END",
            layout: "",
        },
        Command {
            name: "FUNC_POINT_END",
            layout: "",
        },
        Command {
            name: "FUNC_SOUND_DATA",
            layout: "i f f i i f f f f f f",
        },
    ],
};

fn int_of(row: &Eff) -> CoreResult<Value> {
    Ok(json!(row.int(0)?))
}

fn float_of(row: &Eff) -> CoreResult<Value> {
    Ok(float_json(row.float(0)?))
}

/// Takes a row that may appear at most once in a block.
fn single<'a>(cursor: &mut Cursor<'a>, name: &str) -> CoreResult<Option<&'a Eff>> {
    let row = cursor.take(name);
    if row.is_some() && cursor.peek_name() == Some(name) {
        return Err(fail(format!("{name} repeated")));
    }
    Ok(row)
}

fn build_light_set(set: &Eff, cursor: &mut Cursor) -> CoreResult<Value> {
    let fov = single(cursor, "FOV")?.is_some();
    let mut obj = Obj::new().put("index", json!(set.int(0)?));
    if fov {
        obj = obj.put("fov", json!(true));
    }
    for (command, key) in [
        ("BGCOLOR", "bg_color"),
        ("BGCOLOR2", "bg_color2"),
        ("AMBIENT", "ambient"),
    ] {
        obj = obj.opt(key, opt_row(single(cursor, command)?, |r| vec3(r, 0))?);
    }
    let mut lights = Vec::new();
    while let Some(row) = cursor.take("LIGHT") {
        lights.push(
            Obj::new()
                .put("index", json!(row.int(0)?))
                .opt(
                    "direction",
                    if row.has(1) {
                        Some(vec3(row, 1)?)
                    } else {
                        None
                    },
                )
                .opt(
                    "color",
                    if row.has(4) {
                        Some(vec3(row, 4)?)
                    } else {
                        None
                    },
                )
                .done(),
        );
    }
    let mut point_lights = Vec::new();
    while let Some(row) = cursor.take("PLIGHT") {
        point_lights.push(
            json!({"index": row.int(0)?, "power": float_json(row.float(1)?),
                                 "position": vec3(row, 2)?, "color": vec3(row, 5)?}),
        );
    }
    obj = obj
        .put("lights", Value::Array(lights))
        .put("point_lights", Value::Array(point_lights));
    obj = obj.opt(
        "fog_enable",
        opt_row(single(cursor, "FOG_ENABLE")?, int_of)?,
    );
    obj = obj.opt(
        "fog",
        opt_row(single(cursor, "FOG")?, |r| {
            Ok(Obj::new()
                .put("near", float_json(r.float(0)?))
                .put("far", float_json(r.float(1)?))
                .opt(
                    "color",
                    if r.has(2) {
                        Some(ints_json(r, 2, 3)?)
                    } else {
                        None
                    },
                )
                .opt("far_value", slot_json(r, 5))
                .opt("near_value", slot_json(r, 6))
                .done())
        })?,
    );
    cursor.expect("LIGHT_SET_END")?;
    Ok(obj.done())
}

fn build_info(rows: &[Eff]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(rows);
    let mut textures = Vec::new();
    while let Some(row) = cursor.take("IMG") {
        textures.push(text_json(&row.text(0)?));
    }
    let mut models = Vec::new();
    while let Some(row) = cursor.take("PCP") {
        models.push(text_json(&row.text(0)?));
    }
    let mut obj = Obj::new()
        .put("texture_packs", Value::Array(textures))
        .put("model_packs", Value::Array(models))
        .opt(
            "active_light_set",
            opt_row(single(&mut cursor, "ACTIVE_LIGHT_SET")?, int_of)?,
        );
    let mut sets = Vec::new();
    while let Some(set) = cursor.take("LIGHT_SET") {
        sets.push(build_light_set(set, &mut cursor)?);
    }
    obj = obj
        .put("light_sets", Value::Array(sets))
        .opt("floor", opt_row(single(&mut cursor, "FLOOR")?, float_of)?)
        .opt(
            "chara_pos",
            opt_row(single(&mut cursor, "CHARA_POS")?, |r| vec3(r, 0))?,
        )
        .opt(
            "def_foot",
            opt_row(single(&mut cursor, "DEF_FOOT")?, int_of)?,
        )
        .opt(
            "time",
            opt_row(single(&mut cursor, "TIME_FLAG")?, |r| {
                Ok(Obj::new()
                    .put("enable", json!(r.int(0)?))
                    .put("blend", json!(r.int(1)?))
                    .opt("fixed_time", slot_json(r, 2))
                    .opt("fixed_time_enable", slot_json(r, 3))
                    .done())
            })?,
        )
        .opt(
            "time_light_num",
            opt_row(single(&mut cursor, "TIME_LIGHT_NUM")?, int_of)?,
        )
        .opt(
            "time_cfade",
            opt_row(single(&mut cursor, "TIME_CFADE")?, int_of)?,
        )
        .opt(
            "sky",
            opt_row(single(&mut cursor, "SKY_INFO")?, |r| {
                Ok(Obj::new()
                    .put("info", json!(r.int(0)?))
                    .put("height", float_json(r.float(1)?))
                    .opt("sun_angle_degrees", slot_json(r, 2))
                    .done())
            })?,
        )
        .opt(
            "lens_flare",
            opt_row(single(&mut cursor, "LENS_FLARE")?, int_of)?,
        )
        .opt(
            "all_scissor",
            opt_row(single(&mut cursor, "ALL_SCISSOR")?, int_of)?,
        )
        .opt(
            "chara_light_adjust",
            opt_row(single(&mut cursor, "CHARA_LIGHT_ADJUST")?, |r| {
                Ok(json!({"mode": r.int(0)?, "value": vec3(r, 1)?}))
            })?,
        );
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(obj.done())
}

fn flatten_info(info: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    for texture in array(info, "texture_packs")? {
        rows.push(Eff::new("IMG", vec![Some(text_val(texture)?)]));
    }
    for model in array(info, "model_packs")? {
        rows.push(Eff::new("PCP", vec![Some(text_val(model)?)]));
    }
    if let Some(v) = info.get("active_light_set") {
        rows.push(Eff::new("ACTIVE_LIGHT_SET", vec![Some(int_val(v)?)]));
    }
    for set in array(info, "light_sets")? {
        rows.push(Eff::new("LIGHT_SET", vec![Some(int_field(set, "index")?)]));
        if set.get("fov").is_some() {
            rows.push(Eff::new("FOV", vec![]));
        }
        for (command, key) in [
            ("BGCOLOR", "bg_color"),
            ("BGCOLOR2", "bg_color2"),
            ("AMBIENT", "ambient"),
        ] {
            if set.get(key).is_some() {
                rows.push(Eff::new(command, vec3_slots(set, key)?));
            }
        }
        for light in array(set, "lights")? {
            let mut vals = vec![Some(int_field(light, "index")?)];
            vals.extend(opt_vec3_slots(light, "direction")?);
            vals.extend(opt_vec3_slots(light, "color")?);
            rows.push(Eff::new("LIGHT", vals));
        }
        for light in array(set, "point_lights")? {
            let mut vals = vec![
                Some(int_field(light, "index")?),
                Some(float_field(light, "power")?),
            ];
            vals.extend(vec3_slots(light, "position")?);
            vals.extend(vec3_slots(light, "color")?);
            rows.push(Eff::new("PLIGHT", vals));
        }
        if let Some(v) = set.get("fog_enable") {
            rows.push(Eff::new("FOG_ENABLE", vec![Some(int_val(v)?)]));
        }
        if let Some(fog) = set.get("fog") {
            let mut vals = vec![
                Some(float_field(fog, "near")?),
                Some(float_field(fog, "far")?),
            ];
            match fog.get("color") {
                Some(_) => vals.extend(int_slots(fog, "color", 3)?),
                None => vals.extend([None, None, None]),
            }
            vals.push(opt_field(fog, "far_value", float_val)?);
            vals.push(opt_field(fog, "near_value", float_val)?);
            rows.push(Eff::new("FOG", vals));
        }
        rows.push(Eff::new("LIGHT_SET_END", vec![]));
    }
    if let Some(v) = info.get("floor") {
        rows.push(Eff::new("FLOOR", vec![Some(float_val(v)?)]));
    }
    if info.get("chara_pos").is_some() {
        rows.push(Eff::new("CHARA_POS", vec3_slots(info, "chara_pos")?));
    }
    if let Some(v) = info.get("def_foot") {
        rows.push(Eff::new("DEF_FOOT", vec![Some(int_val(v)?)]));
    }
    if let Some(time) = info.get("time") {
        rows.push(Eff::new(
            "TIME_FLAG",
            vec![
                Some(int_field(time, "enable")?),
                Some(int_field(time, "blend")?),
                opt_field(time, "fixed_time", float_val)?,
                opt_field(time, "fixed_time_enable", int_val)?,
            ],
        ));
    }
    if let Some(v) = info.get("time_light_num") {
        rows.push(Eff::new("TIME_LIGHT_NUM", vec![Some(int_val(v)?)]));
    }
    if let Some(v) = info.get("time_cfade") {
        rows.push(Eff::new("TIME_CFADE", vec![Some(int_val(v)?)]));
    }
    if let Some(sky) = info.get("sky") {
        rows.push(Eff::new(
            "SKY_INFO",
            vec![
                Some(int_field(sky, "info")?),
                Some(float_field(sky, "height")?),
                opt_field(sky, "sun_angle_degrees", float_val)?,
            ],
        ));
    }
    if let Some(v) = info.get("lens_flare") {
        rows.push(Eff::new("LENS_FLARE", vec![Some(int_val(v)?)]));
    }
    if let Some(v) = info.get("all_scissor") {
        rows.push(Eff::new("ALL_SCISSOR", vec![Some(int_val(v)?)]));
    }
    if let Some(adjust) = info.get("chara_light_adjust") {
        let mut vals = vec![Some(int_field(adjust, "mode")?)];
        vals.extend(vec3_slots(adjust, "value")?);
        rows.push(Eff::new("CHARA_LIGHT_ADJUST", vals));
    }
    Ok(())
}

/// Function-point data rows, their document keys and row keys, in document order.
const FUNC_DATA_ROWS: RowTable = &[
    (
        "FUNC_FIRE_DATA",
        "fire",
        &[
            ("red", 'f'),
            ("green", 'f'),
            ("blue", 'f'),
            ("effect", 'i'),
            ("heat_haze", 'i'),
            ("cast_light", 'i'),
        ],
    ),
    (
        "FUNC_PLIGHT_DATA",
        "plight",
        &[
            ("power", 'f'),
            ("red", 'f'),
            ("green", 'f'),
            ("blue", 'f'),
            ("light_type", 'i'),
            ("light_chara", 'i'),
            ("unk_40", 'i'),
            ("unk_44", 'i'),
            ("no_map_light", 'i'),
            ("flicker_type", 'i'),
            ("flicker_depth", 'f'),
            ("flicker_period", 'f'),
        ],
    ),
    (
        "FUNC_ANIME_DATA",
        "anime",
        &[
            ("parts_name", 's'),
            ("piece_name", 's'),
            ("frame_name", 's'),
            ("kind", 'i'),
            ("mode", 'i'),
            ("param_x", 'f'),
            ("param_y", 'f'),
            ("param_z", 'f'),
            ("speed_x", 'f'),
            ("speed_y", 'f'),
            ("speed_z", 'f'),
            ("end_x", 'f'),
            ("end_y", 'f'),
            ("end_z", 'f'),
            ("uniform", 'i'),
            ("piece_space", 'i'),
        ],
    ),
    (
        "FUNC_INVENT_DATA",
        "invent",
        &[
            ("neta_no", 'i'),
            ("min_x", 'f'),
            ("min_y", 'f'),
            ("min_z", 'f'),
            ("max_x", 'f'),
            ("max_y", 'f'),
            ("max_z", 'f'),
            ("unk_24", 'i'),
            ("range", 'f'),
            ("angle_degrees", 'f'),
        ],
    ),
    (
        "FUNC_EVENT_DATA",
        "event",
        &[
            ("kind", 's'),
            ("event_no", 'i'),
            ("point_no", 'i'),
            ("arg1", 'i'),
            ("arg2", 'i'),
            ("arg3", 'i'),
            ("target", 's'),
            ("action", 'i'),
            ("item", 'i'),
            ("unk_100", 'i'),
        ],
    ),
    (
        "FUNC_SOUND_DATA",
        "sound",
        &[
            ("se_no", 'i'),
            ("near_dist", 'f'),
            ("far_dist", 'f'),
            ("unk_2c", 'i'),
            ("shape", 'i'),
            ("start_x", 'f'),
            ("start_y", 'f'),
            ("start_z", 'f'),
            ("end_x", 'f'),
            ("end_y", 'f'),
            ("end_z", 'f'),
        ],
    ),
    ("FUNC_EFFECT_NAME", "effect", &[("name", 's')]),
];

fn keyed(row: &Eff, keys: &[(&str, char)]) -> Value {
    let mut obj = Obj::new();
    for (slot, (key, _)) in keys.iter().enumerate() {
        obj = obj.opt(key, slot_json(row, slot));
    }
    obj.done()
}

fn transform(row: &Eff) -> CoreResult<Value> {
    Ok(json!({"position": vec3(row, 0)?, "rotation": vec3(row, 3)?, "scale": vec3(row, 6)?}))
}

fn transform_slots(value: &Value) -> CoreResult<Vec<Option<Val>>> {
    let mut vals = vec3_slots(value, "position")?;
    vals.extend(vec3_slots(value, "rotation")?);
    vals.extend(vec3_slots(value, "scale")?);
    Ok(vals)
}

fn build_func_points(cursor: &mut Cursor) -> CoreResult<Value> {
    let mut points = Vec::new();
    while let Some(data) = cursor.take("FUNC_DATA") {
        let mut obj = Obj::new()
            .put("type", text_json(&data.text(0)?))
            .put("enable", json!(data.int(1)?))
            .opt(
                "name",
                opt_row(single(cursor, "FUNC_NAME")?, |r| Ok(text_json(&r.text(0)?)))?,
            )
            .opt(
                "flag",
                opt_row(single(cursor, "FUNC_FLAG")?, |r| {
                    Ok(json!({"flag_no": r.int(0)?, "unk_8": r.int(1)?,
                              "start": float_json(r.float(2)?), "end": float_json(r.float(3)?)}))
                })?,
            );
        for (command, key, keys) in FUNC_DATA_ROWS {
            obj = obj.opt(key, single(cursor, command)?.map(|r| keyed(r, keys)));
        }
        obj = obj.opt(
            "transform",
            opt_row(single(cursor, "FUNC_POS")?, transform)?,
        );
        cursor.expect("FUNC_DATA_END")?;
        points.push(obj.done());
    }
    cursor.expect("FUNC_POINT_END")?;
    Ok(Value::Array(points))
}

fn flatten_func_points(points: &[Value], rows: &mut Vec<Eff>) -> CoreResult<()> {
    rows.push(Eff::new("FUNC_POINT", vec![]));
    for point in points {
        rows.push(Eff::new(
            "FUNC_DATA",
            vec![
                Some(text_field(point, "type")?),
                Some(int_field(point, "enable")?),
            ],
        ));
        if let Some(name) = point.get("name") {
            rows.push(Eff::new("FUNC_NAME", vec![Some(text_val(name)?)]));
        }
        if let Some(flag) = point.get("flag") {
            rows.push(Eff::new(
                "FUNC_FLAG",
                vec![
                    Some(int_field(flag, "flag_no")?),
                    Some(int_field(flag, "unk_8")?),
                    Some(float_field(flag, "start")?),
                    Some(float_field(flag, "end")?),
                ],
            ));
        }
        for (command, key, keys) in FUNC_DATA_ROWS {
            if let Some(data) = point.get(*key) {
                rows.push(row_of(data, command, keys)?);
            }
        }
        if let Some(t) = point.get("transform") {
            rows.push(Eff::new("FUNC_POS", transform_slots(t)?));
        }
        rows.push(Eff::new("FUNC_DATA_END", vec![]));
    }
    rows.push(Eff::new("FUNC_POINT_END", vec![]));
    Ok(())
}

fn build_piece(piece: &Eff, cursor: &mut Cursor) -> CoreResult<Value> {
    let mut obj = Obj::new()
        .put("model", text_json(&piece.text(0)?))
        .opt("show", slot_json(piece, 1))
        .opt(
            "name",
            opt_row(single(cursor, "PIECE_NAME")?, |r| {
                Ok(text_json(&r.text(0)?))
            })?,
        );
    for (command, key) in [
        ("PIECE_POS", "position"),
        ("PIECE_ROT", "rotation"),
        ("PIECE_SCALE", "scale"),
    ] {
        obj = obj.opt(key, opt_row(single(cursor, command)?, |r| vec3(r, 0))?);
    }
    if let Some(start) = single(cursor, "PIECE_MATERIAL_START")? {
        let mut entries = Vec::new();
        while let Some(row) = cursor.take("PIECE_MATERIAL") {
            entries.push(
                json!({"frame": text_json(&row.text(0)?), "material_no": row.int(1)?,
                                "color": floats_json(row, 2, 4)?, "color_no": row.int(6)?}),
            );
        }
        obj = obj.put(
            "materials",
            json!({"count": start.int(0)?, "entries": entries}),
        );
    }
    obj = obj
        .opt(
            "collision",
            opt_row(single(cursor, "PIECE_COL_TYPE")?, |r| {
                Ok(Obj::new()
                    .put("type", json!(r.int(0)?))
                    .opt("param", slot_json(r, 1))
                    .done())
            })?,
        )
        .opt(
            "time",
            opt_row(single(cursor, "PIECE_TIME")?, |r| floats_json(r, 0, 2))?,
        );
    cursor.expect("PIECE_END")?;
    Ok(obj.done())
}

fn flatten_piece(piece: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    rows.push(Eff::new(
        "PIECE",
        vec![
            Some(text_field(piece, "model")?),
            opt_field(piece, "show", int_val)?,
        ],
    ));
    if let Some(name) = piece.get("name") {
        rows.push(Eff::new("PIECE_NAME", vec![Some(text_val(name)?)]));
    }
    for (command, key) in [
        ("PIECE_POS", "position"),
        ("PIECE_ROT", "rotation"),
        ("PIECE_SCALE", "scale"),
    ] {
        if piece.get(key).is_some() {
            rows.push(Eff::new(command, vec3_slots(piece, key)?));
        }
    }
    if let Some(materials) = piece.get("materials") {
        rows.push(Eff::new(
            "PIECE_MATERIAL_START",
            vec![Some(int_field(materials, "count")?)],
        ));
        for entry in array(materials, "entries")? {
            let mut vals = vec![
                Some(text_field(entry, "frame")?),
                Some(int_field(entry, "material_no")?),
            ];
            vals.extend(float_slots(entry, "color", 4)?);
            vals.push(Some(int_field(entry, "color_no")?));
            rows.push(Eff::new("PIECE_MATERIAL", vals));
        }
    }
    if let Some(collision) = piece.get("collision") {
        rows.push(Eff::new(
            "PIECE_COL_TYPE",
            vec![
                Some(int_field(collision, "type")?),
                opt_field(collision, "param", int_val)?,
            ],
        ));
    }
    if piece.get("time").is_some() {
        rows.push(Eff::new("PIECE_TIME", float_slots(piece, "time", 2)?));
    }
    rows.push(Eff::new("PIECE_END", vec![]));
    Ok(())
}

fn build_part(part: &Eff, cursor: &mut Cursor) -> CoreResult<Value> {
    let mut obj = Obj::new()
        .put("name", text_json(&part.text(0)?))
        .opt(
            "far_clip",
            opt_row(single(cursor, "FAR_CLIP")?, |r| {
                Ok(json!({"distance": float_json(r.float(0)?), "fade": r.int(1)?}))
            })?,
        )
        .opt(
            "light_flag",
            opt_row(single(cursor, "LIGHT_FLAG")?, |r| {
                Ok(json!({"no_light": r.int(0)?, "no_plight": r.int(1)?}))
            })?,
        )
        .opt(
            "move_flag",
            opt_row(single(cursor, "MOVE_FLAG")?, |r| ints_json(r, 0, 4))?,
        );
    if matches!(
        cursor.peek_name(),
        Some("LOD_START" | "LOD_BLEND" | "LOD_PIECE" | "LOD_END")
    ) {
        return Err(fail("map part levels of detail are not supported"));
    }
    let mut pieces = Vec::new();
    while let Some(piece) = cursor.take("PIECE") {
        pieces.push(build_piece(piece, cursor)?);
    }
    obj = obj.put("pieces", Value::Array(pieces));
    if cursor.take("FUNC_POINT").is_some() {
        obj = obj.put("func_points", build_func_points(cursor)?);
    }
    cursor.expect("PARTS_END")?;
    Ok(obj.done())
}

fn flatten_part(part: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    rows.push(Eff::new("PARTS", vec![Some(text_field(part, "name")?)]));
    if let Some(clip) = part.get("far_clip") {
        rows.push(Eff::new(
            "FAR_CLIP",
            vec![
                Some(float_field(clip, "distance")?),
                Some(int_field(clip, "fade")?),
            ],
        ));
    }
    if let Some(flag) = part.get("light_flag") {
        rows.push(Eff::new(
            "LIGHT_FLAG",
            vec![
                Some(int_field(flag, "no_light")?),
                Some(int_field(flag, "no_plight")?),
            ],
        ));
    }
    if part.get("move_flag").is_some() {
        rows.push(Eff::new("MOVE_FLAG", int_slots(part, "move_flag", 4)?));
    }
    for piece in array(part, "pieces")? {
        flatten_piece(piece, rows)?;
    }
    if part.get("func_points").is_some() {
        flatten_func_points(array(part, "func_points")?, rows)?;
    }
    rows.push(Eff::new("PARTS_END", vec![]));
    Ok(())
}

fn build_placement(row: &Eff, cursor: &mut Cursor) -> CoreResult<Value> {
    let mut obj = Obj::new()
        .put("name", text_json(&row.text(0)?))
        .opt("show", slot_json(row, 1))
        .opt(
            "far_clip",
            opt_row(single(cursor, "MAP_FAR_CLIP")?, |r| {
                Ok(json!({"distance": float_json(r.float(0)?), "fade": r.int(1)?}))
            })?,
        )
        .opt(
            "definition",
            opt_row(single(cursor, "PARTS_NAME")?, |r| {
                Ok(text_json(&r.text(0)?))
            })?,
        )
        .opt(
            "group",
            opt_row(single(cursor, "PARTS_GROUP")?, |r| {
                Ok(text_json(&r.text(0)?))
            })?,
        );
    for (command, key) in [
        ("PARTS_POS", "position"),
        ("PARTS_ROT", "rotation"),
        ("PARTS_SCALE", "scale"),
    ] {
        obj = obj.opt(key, opt_row(single(cursor, command)?, |r| vec3(r, 0))?);
    }
    cursor.expect("MAP_PARTS_END")?;
    Ok(obj.done())
}

fn flatten_placement(placement: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    rows.push(Eff::new(
        "MAP_PARTS",
        vec![
            Some(text_field(placement, "name")?),
            opt_field(placement, "show", int_val)?,
        ],
    ));
    if let Some(clip) = placement.get("far_clip") {
        rows.push(Eff::new(
            "MAP_FAR_CLIP",
            vec![
                Some(float_field(clip, "distance")?),
                Some(int_field(clip, "fade")?),
            ],
        ));
    }
    if let Some(v) = placement.get("definition") {
        rows.push(Eff::new("PARTS_NAME", vec![Some(text_val(v)?)]));
    }
    if let Some(v) = placement.get("group") {
        rows.push(Eff::new("PARTS_GROUP", vec![Some(text_val(v)?)]));
    }
    for (command, key) in [
        ("PARTS_POS", "position"),
        ("PARTS_ROT", "rotation"),
        ("PARTS_SCALE", "scale"),
    ] {
        if placement.get(key).is_some() {
            rows.push(Eff::new(command, vec3_slots(placement, key)?));
        }
    }
    rows.push(Eff::new("MAP_PARTS_END", vec![]));
    Ok(())
}

fn build_cameras(info: &Eff, cursor: &mut Cursor) -> CoreResult<Value> {
    let mut cameras = Vec::new();
    while cursor.take("FIX_CAMERA").is_some() {
        let mut obj = Obj::new().opt(
            "position",
            opt_row(single(cursor, "FIX_CAMERA_POS")?, |r| vec3(r, 0))?,
        );
        let mut positions = Vec::new();
        while let Some(row) = cursor.take("FIX_CAMERA_POS2") {
            positions.push(json!({"index": row.int(0)?, "position": vec3(row, 1)?}));
        }
        let mut groups = Vec::new();
        while let Some(row) = cursor.take("FIX_CAMERA_OFF_GROUP") {
            groups.push(json!({"index": row.int(0)?, "group": text_json(&row.text(1)?), "unk_4": row.int(2)?}));
        }
        let mut rects = Vec::new();
        while let Some(row) = cursor.take("FIX_CAMERA_RECT") {
            rects.push(
                Obj::new()
                    .put("shape", text_json(&row.text(0)?))
                    .put("min", vec3(row, 1)?)
                    .put("max", vec3(row, 4)?)
                    .opt(
                        "position",
                        if row.has(7) {
                            Some(vec3(row, 7)?)
                        } else {
                            None
                        },
                    )
                    .opt(
                        "rotation",
                        if row.has(10) {
                            Some(vec3(row, 10)?)
                        } else {
                            None
                        },
                    )
                    .opt(
                        "scale",
                        if row.has(13) {
                            Some(vec3(row, 13)?)
                        } else {
                            None
                        },
                    )
                    .done(),
            );
        }
        obj = obj
            .put("positions", Value::Array(positions))
            .put("draw_groups", Value::Array(groups))
            .put("rects", Value::Array(rects));
        cursor.expect("FIX_CAMERA_END")?;
        cameras.push(obj.done());
    }
    Ok(json!({"count": info.int(0)?, "cameras": cameras}))
}

fn flatten_cameras(cameras: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    rows.push(Eff::new(
        "CAMERA_INFO",
        vec![Some(int_field(cameras, "count")?)],
    ));
    for camera in array(cameras, "cameras")? {
        rows.push(Eff::new("FIX_CAMERA", vec![]));
        if camera.get("position").is_some() {
            rows.push(Eff::new("FIX_CAMERA_POS", vec3_slots(camera, "position")?));
        }
        for pos in array(camera, "positions")? {
            let mut vals = vec![Some(int_field(pos, "index")?)];
            vals.extend(vec3_slots(pos, "position")?);
            rows.push(Eff::new("FIX_CAMERA_POS2", vals));
        }
        for group in array(camera, "draw_groups")? {
            rows.push(Eff::new(
                "FIX_CAMERA_OFF_GROUP",
                vec![
                    Some(int_field(group, "index")?),
                    Some(text_field(group, "group")?),
                    Some(int_field(group, "unk_4")?),
                ],
            ));
        }
        for rect in array(camera, "rects")? {
            let mut vals = vec![Some(text_field(rect, "shape")?)];
            vals.extend(vec3_slots(rect, "min")?);
            vals.extend(vec3_slots(rect, "max")?);
            vals.extend(opt_vec3_slots(rect, "position")?);
            vals.extend(opt_vec3_slots(rect, "rotation")?);
            vals.extend(opt_vec3_slots(rect, "scale")?);
            rows.push(Eff::new("FIX_CAMERA_RECT", vals));
        }
        rows.push(Eff::new("FIX_CAMERA_END", vec![]));
    }
    Ok(())
}

fn build_scene(rows: &[Eff]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(rows);
    let mut parts = Vec::new();
    while let Some(part) = cursor.take("PARTS") {
        parts.push(build_part(part, &mut cursor)?);
    }
    let mut placements = Vec::new();
    while let Some(row) = cursor.take("MAP_PARTS") {
        placements.push(build_placement(row, &mut cursor)?);
    }
    let mut obj = Obj::new()
        .put("parts", Value::Array(parts))
        .put("placements", Value::Array(placements));
    if let Some(info) = cursor.take("CAMERA_INFO") {
        obj = obj.put("cameras", build_cameras(info, &mut cursor)?);
    }
    if cursor.take("FUNC_POINT").is_some() {
        obj = obj.put("func_points", build_func_points(&mut cursor)?);
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(obj.done())
}

fn flatten_scene(document: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    for part in array(document, "parts")? {
        flatten_part(part, rows)?;
    }
    for placement in array(document, "placements")? {
        flatten_placement(placement, rows)?;
    }
    if let Some(cameras) = document.get("cameras") {
        flatten_cameras(cameras, rows)?;
    }
    if document.get("func_points").is_some() {
        flatten_func_points(array(document, "func_points")?, rows)?;
    }
    Ok(())
}

/// Puts the map-information values in the order the document gives them back: the texture packs,
/// then the model packs (each list keeps its order; the two never interact), and within each lighting
/// set the directional lights before the point lights, which fill separate tables.
pub(super) fn canonical_map_info(rows: Vec<Eff>) -> Vec<Eff> {
    let (textures, rest): (Vec<_>, Vec<_>) = rows.into_iter().partition(|r| r.name == "IMG");
    let (models, rest): (Vec<_>, Vec<_>) = rest.into_iter().partition(|r| r.name == "PCP");
    let mut out = textures;
    out.extend(models);
    let mut set: Vec<Eff> = Vec::new();
    let mut in_set = false;
    for row in rest {
        if row.name == "LIGHT_SET" {
            in_set = true;
        }
        if !in_set {
            out.push(row);
            continue;
        }
        let end = row.name == "LIGHT_SET_END";
        set.push(row);
        if end {
            let (points, others): (Vec<_>, Vec<_>) =
                set.drain(..).partition(|r| r.name == "PLIGHT");
            let at = others
                .iter()
                .rposition(|r| r.name == "LIGHT")
                .or_else(|| others.iter().rposition(|r| r.name == "AMBIENT"))
                .map_or(others.len().saturating_sub(1), |p| p + 1);
            let mut merged = others;
            merged.splice(at..at, points);
            out.extend(merged);
            in_set = false;
        }
    }
    out.extend(set);
    out
}

fn build_scene_map(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let info = build_info(&canonical_map_info(passes[0].clone()))?;
    let scene = build_scene(&passes[1])?;
    let Value::Object(mut obj) = scene else {
        unreachable!()
    };
    obj.insert("info".into(), info);
    Ok(Value::Object(obj))
}

fn flatten_scene_map(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut info = Vec::new();
    flatten_info(field(document, "info")?, &mut info)?;
    let mut scene = Vec::new();
    flatten_scene(document, &mut scene)?;
    Ok(vec![info, scene])
}

const SCENE_MAP: Family = Family {
    kind: "scene_map",
    passes: &[&MAPINFO, &MAP],
    ignored: &[],
    build: build_scene_map,
    flatten: flatten_scene_map,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[&MODEL_LIST, &SKY_FAMILY, &SCENE_MAP];

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> Value {
        let document = super::super::super::configs::parse(script).unwrap();
        super::super::build(alias, &document).unwrap().0
    }

    #[test]
    fn model_lists_keep_models_types_and_clips() {
        let doc = convert(
            "map/a.pcp/info.cfg",
            b"PCP_NAME \"a\"; MDS \"m.gltf\",\"m\"; TYPE 1; FAR_CLIP 900,1; MDS_END; MDS \"n.gltf\"; MDS_END;",
        );
        assert_eq!(doc["kind"], "model_list");
        assert_eq!(
            doc["models"][0],
            json!({"model":"m.gltf","type":1,"far_clip":{"distance":900.0,"fade":1}})
        );
        assert_eq!(doc["models"][1], json!({"model":"n.gltf"}));
    }

    #[test]
    fn skies_group_rows_by_table() {
        let doc = convert(
            "map/s.sky/info.cfg",
            b"SKY_IMG 0,\"a.json\"; SKY_MDS 0,\"s.gltf\",0.5; SKY_BG \"bg.gltf\"; SKY_ANIME 1,\"c\",2.0;",
        );
        assert_eq!(doc["models"][0]["rotation_degrees"], 0.5);
        assert_eq!(doc["backgrounds"][0]["file"], "bg.gltf");
        assert_eq!(doc["animations"][0]["band"], 1);
    }

    #[test]
    fn scene_maps_nest_settings_parts_placements_and_func_points() {
        let doc = convert(
            "map/s/s01/s01.map",
            b"IMG \"t.json\"; PCP \"p.pcp\"; IMG \"u.json\"; LIGHT_SET 0; FOV 1; BGCOLOR 1,2,3; PLIGHT 0,2,1,2,3,4,5,6; LIGHT 0,0,-1,0; FOG 10,20; LIGHT_SET_END;
              TIME_FLAG 1,1; PARTS \"p\"; LIGHT_FLAG 0,1; PIECE \"m.gltf\",1; PIECE_NAME \"m0\"; PIECE_POS 1,2,3; PIECE_END;
              FUNC_POINT 1; FUNC_DATA \"event\",1; FUNC_NAME \"e\"; FUNC_EVENT_DATA \"door\",0,5,1,2,3; FUNC_POS 0,0,0,0,0,0,1,1,1; FUNC_DATA_END; FUNC_POINT_END;
              PARTS_END; MAP_PARTS \"q\"; PARTS_NAME \"p\"; PARTS_POS 0,0,0; MAP_PARTS_END;
              CAMERA_INFO 1; FIX_CAMERA; FIX_CAMERA_POS 1,2,3; FIX_CAMERA_RECT \"box\",0,0,0,1,1,1; FIX_CAMERA_END; CAMERA_INFO_END;",
        );
        assert_eq!(doc["info"]["texture_packs"], json!(["t.json", "u.json"]));
        assert_eq!(
            doc["info"]["light_sets"][0]["lights"][0]["direction"],
            json!([0.0, -1.0, 0.0])
        );
        assert!(
            doc["info"]["light_sets"][0]["lights"][0]
                .get("color")
                .is_none()
        );
        assert_eq!(
            doc["info"]["light_sets"][0]["point_lights"][0]["power"],
            2.0
        );
        assert!(doc["info"]["light_sets"][0]["fog"].get("color").is_none());
        assert_eq!(doc["info"]["time"], json!({"enable":1,"blend":1}));
        assert_eq!(
            doc["parts"][0]["pieces"][0]["position"],
            json!([1.0, 2.0, 3.0])
        );
        assert_eq!(doc["parts"][0]["func_points"][0]["event"]["kind"], "door");
        assert!(
            doc["parts"][0]["func_points"][0]["event"]
                .get("target")
                .is_none()
        );
        assert_eq!(doc["placements"][0]["definition"], "p");
        assert_eq!(
            doc["cameras"]["cameras"][0]["rects"][0]["max"],
            json!([1.0, 1.0, 1.0])
        );
    }
}
