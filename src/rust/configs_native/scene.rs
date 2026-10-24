//! Scene configurations (`map/**/<map>.cfg`): draw-off rectangles, occlusion planes, event points and
//! water, read by `CMap::LoadCfgFile` (mapload.cpp), and for georama maps the editor parts read by
//! `CEditMap::LoadEditInfo` (editmap.cpp) from the same file.
use super::*;

const CFG: Pass = Pass {
    name: "cfg_tag",
    commands: &[
        Command {
            name: "DRAW_OFF_RECT",
            layout: "f f f f f f f f f f f f",
        },
        Command {
            name: "OCCLUSION_PLANE",
            layout: "f f f f f f f f f f f f",
        },
        Command {
            name: "FUNC_DATA",
            layout: "s",
        },
        Command {
            name: "FUNC_NAME",
            layout: "s",
        },
        Command {
            name: "FUNC_EVENT_DATA",
            layout: "i s?",
        },
        Command {
            name: "FUNC_POS",
            layout: "f f f f f f f f f",
        },
        Command {
            name: "WATER_SURFACE_NUM",
            layout: "i",
        },
        Command {
            name: "WATER_SURFACE_START",
            layout: "-",
        },
        Command {
            name: "WATER_VERTEX",
            layout: "i i f f f f f f",
        },
        Command {
            name: "WATER_POS",
            layout: "f f f",
        },
        Command {
            name: "WATER_PARAM",
            layout: "f f f f",
        },
        Command {
            name: "WATER_SHAKE",
            layout: "-",
        },
        Command {
            name: "WATER_SURFACE_END",
            layout: "",
        },
        Command {
            name: "WATER_DRAW_NUM",
            layout: "i",
        },
        Command {
            name: "WATER_DRAW",
            layout: "i s f? f? f?",
        },
    ],
};

const EMAP: Pass = Pass {
    name: "emap_tag",
    commands: &[
        Command {
            name: "FIX_EPARTS_START",
            layout: "i",
        },
        Command {
            name: "FIX_EPARTS",
            layout: "i f f f i",
        },
        Command {
            name: "FIX_EPARTS_END",
            layout: "",
        },
        Command {
            name: "EDIT_RIVER",
            layout: "-",
        },
        Command {
            name: "RIVER_PARTS_NAME",
            layout: "i s s",
        },
        Command {
            name: "WATER_PARTS_NAME",
            layout: "s",
        },
        Command {
            name: "EDIT_RIVER_END",
            layout: "-",
        },
        Command {
            name: "INIT_EPARTS_START",
            layout: "i",
        },
        Command {
            name: "INIT_EPARTS",
            layout: "i f f f i",
        },
        Command {
            name: "INIT_EPARTS_END",
            layout: "",
        },
        Command {
            name: "MASK_PARTS_NAME",
            layout: "i s",
        },
    ],
};

/// Rank of a command among the independent tables of each pass, which the document stores apart.
fn group(name: &str) -> usize {
    match name {
        "DRAW_OFF_RECT" => 0,
        "OCCLUSION_PLANE" => 1,
        n if n.starts_with("WATER_") && n != "WATER_PARTS_NAME" => 2,
        n if n.starts_with("FUNC_") => 3,
        n if n.starts_with("FIX_EPARTS") => 0,
        "RIVER_PARTS_NAME" => 1,
        "WATER_PARTS_NAME" => 2,
        "MASK_PARTS_NAME" => 3,
        n if n.starts_with("INIT_EPARTS") => 4,
        _ => 5,
    }
}

/// Orders a pass's values as the document gives them back: each independent table in turn, keeping
/// every table's own order.
pub(super) fn canonical_scene_config(rows: Vec<Eff>) -> Vec<Eff> {
    let mut rows = rows;
    rows.sort_by_key(|row| group(&row.name));
    rows
}

fn vec3(row: &Eff, from: usize) -> CoreResult<Value> {
    floats_json(row, from, 3)
}

fn opt_row<T>(
    row: Option<&Eff>,
    convert: impl FnOnce(&Eff) -> CoreResult<T>,
) -> CoreResult<Option<T>> {
    row.map(convert).transpose()
}

fn build_cfg(rows: &[Eff]) -> CoreResult<Obj> {
    let mut cursor = Cursor::new(rows);
    let mut rects = Vec::new();
    while let Some(row) = cursor.take("DRAW_OFF_RECT") {
        rects.push(json!({"box": {"min": vec3(row, 0)?, "max": vec3(row, 3)?},
                          "box2": {"min": vec3(row, 6)?, "max": vec3(row, 9)?}}));
    }
    let mut planes = Vec::new();
    while let Some(row) = cursor.take("OCCLUSION_PLANE") {
        planes.push(json!([
            vec3(row, 0)?,
            vec3(row, 3)?,
            vec3(row, 6)?,
            vec3(row, 9)?
        ]));
    }
    let mut obj = Obj::new()
        .put("draw_off_rects", Value::Array(rects))
        .put("occlusion_planes", Value::Array(planes));
    let mut water = Obj::new().opt(
        "surface_count",
        opt_row(cursor.take("WATER_SURFACE_NUM"), |r| Ok(json!(r.int(0)?)))?,
    );
    let mut surfaces = Vec::new();
    while matches!(
        cursor.peek_name(),
        Some("WATER_VERTEX" | "WATER_POS" | "WATER_PARAM" | "WATER_SURFACE_END")
    ) {
        let surface = Obj::new()
            .opt(
                "vertex",
                opt_row(cursor.take("WATER_VERTEX"), |r| {
                    Ok(json!({"count_x": r.int(0)?, "count_z": r.int(1)?,
                              "corner0": vec3(r, 2)?, "corner1": vec3(r, 5)?}))
                })?,
            )
            .opt(
                "position",
                opt_row(cursor.take("WATER_POS"), |r| vec3(r, 0))?,
            )
            .opt(
                "param",
                opt_row(cursor.take("WATER_PARAM"), |r| floats_json(r, 0, 4))?,
            );
        cursor.expect("WATER_SURFACE_END")?;
        surfaces.push(surface.done());
    }
    water = water.put("surfaces", Value::Array(surfaces)).opt(
        "draw_count",
        opt_row(cursor.take("WATER_DRAW_NUM"), |r| Ok(json!(r.int(0)?)))?,
    );
    let mut draws = Vec::new();
    while let Some(row) = cursor.take("WATER_DRAW") {
        draws.push(
            Obj::new()
                .put("surface", json!(row.int(0)?))
                .put("target", text_json(&row.text(1)?))
                .opt(
                    "position",
                    if row.has(2) {
                        Some(vec3(row, 2)?)
                    } else {
                        None
                    },
                )
                .done(),
        );
    }
    water = water.put("draws", Value::Array(draws));
    obj = obj.put("water", water.done());
    let mut points = Vec::new();
    while let Some(data) = cursor.take("FUNC_DATA") {
        let point = Obj::new()
            .put("type", text_json(&data.text(0)?))
            .opt("name", opt_row(cursor.take("FUNC_NAME"), |r| Ok(text_json(&r.text(0)?)))?)
            .opt(
                "event",
                opt_row(cursor.take("FUNC_EVENT_DATA"), |r| {
                    Ok(Obj::new()
                        .put("point_no", json!(r.int(0)?))
                        .opt("mode", r.vals[1].as_ref().map(val_json))
                        .done())
                })?,
            )
            .opt(
                "transform",
                opt_row(cursor.take("FUNC_POS"), |r| {
                    Ok(json!({"position": vec3(r, 0)?, "rotation": vec3(r, 3)?, "scale": vec3(r, 6)?}))
                })?,
            );
        points.push(point.done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(obj.put("event_points", Value::Array(points)))
}

fn place_rows(
    cursor: &mut Cursor,
    start: &str,
    entry: &str,
    end: &str,
) -> CoreResult<Option<Value>> {
    let Some(head) = cursor.take(start) else {
        return Ok(None);
    };
    let mut parts = Vec::new();
    while let Some(row) = cursor.take(entry) {
        parts.push(json!({"id": row.int(0)?, "position": vec3(row, 1)?, "angle": row.int(4)?}));
    }
    cursor.expect(end)?;
    Ok(Some(json!({"count": head.int(0)?, "parts": parts})))
}

fn build_georama(rows: &[Eff]) -> CoreResult<Option<Value>> {
    if rows.is_empty() {
        return Ok(None);
    }
    let mut cursor = Cursor::new(rows);
    let mut obj = Obj::new().opt(
        "fixed_parts",
        place_rows(
            &mut cursor,
            "FIX_EPARTS_START",
            "FIX_EPARTS",
            "FIX_EPARTS_END",
        )?,
    );
    let mut rivers = Vec::new();
    while let Some(row) = cursor.take("RIVER_PARTS_NAME") {
        rivers.push(json!({"index": row.int(0)?, "parts": text_json(&row.text(1)?), "model": text_json(&row.text(2)?)}));
    }
    obj = obj.put("river_parts", Value::Array(rivers));
    let mut water = Vec::new();
    while let Some(row) = cursor.take("WATER_PARTS_NAME") {
        water.push(text_json(&row.text(0)?));
    }
    obj = obj.put("water_parts_models", Value::Array(water));
    let mut masks = Vec::new();
    while let Some(row) = cursor.take("MASK_PARTS_NAME") {
        masks.push(json!({"index": row.int(0)?, "model": text_json(&row.text(1)?)}));
    }
    obj = obj.put("mask_parts", Value::Array(masks)).opt(
        "initial_parts",
        place_rows(
            &mut cursor,
            "INIT_EPARTS_START",
            "INIT_EPARTS",
            "INIT_EPARTS_END",
        )?,
    );
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(Some(obj.done()))
}

fn build_scene_config(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let cfg = build_cfg(&canonical_scene_config(passes[0].clone()))?;
    let georama = build_georama(&canonical_scene_config(passes[1].clone()))?;
    Ok(cfg.opt("georama", georama).done())
}

fn vec3_slots(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    float_slots(value, key, 3)
}

fn flatten_cfg(document: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    for rect in array(document, "draw_off_rects")? {
        let mut vals = Vec::new();
        for key in ["box", "box2"] {
            let b = field(rect, key)?;
            vals.extend(vec3_slots(b, "min")?);
            vals.extend(vec3_slots(b, "max")?);
        }
        rows.push(Eff::new("DRAW_OFF_RECT", vals));
    }
    for plane in array(document, "occlusion_planes")? {
        let corners = plane
            .as_array()
            .filter(|c| c.len() == 4)
            .ok_or_else(|| fail("a plane has four corners"))?;
        let mut vals = Vec::new();
        for corner in corners {
            let xyz = corner
                .as_array()
                .filter(|c| c.len() == 3)
                .ok_or_else(|| fail("a corner has three numbers"))?;
            for v in xyz {
                vals.push(Some(float_val(v)?));
            }
        }
        rows.push(Eff::new("OCCLUSION_PLANE", vals));
    }
    let water = field(document, "water")?;
    if let Some(count) = water.get("surface_count") {
        rows.push(Eff::new("WATER_SURFACE_NUM", vec![Some(int_val(count)?)]));
    }
    for surface in array(water, "surfaces")? {
        if let Some(vertex) = surface.get("vertex") {
            let mut vals = vec![
                Some(int_field(vertex, "count_x")?),
                Some(int_field(vertex, "count_z")?),
            ];
            vals.extend(vec3_slots(vertex, "corner0")?);
            vals.extend(vec3_slots(vertex, "corner1")?);
            rows.push(Eff::new("WATER_VERTEX", vals));
        }
        if surface.get("position").is_some() {
            rows.push(Eff::new("WATER_POS", vec3_slots(surface, "position")?));
        }
        if surface.get("param").is_some() {
            rows.push(Eff::new("WATER_PARAM", float_slots(surface, "param", 4)?));
        }
        rows.push(Eff::new("WATER_SURFACE_END", vec![]));
    }
    if let Some(count) = water.get("draw_count") {
        rows.push(Eff::new("WATER_DRAW_NUM", vec![Some(int_val(count)?)]));
    }
    for draw in array(water, "draws")? {
        let mut vals = vec![
            Some(int_field(draw, "surface")?),
            Some(text_field(draw, "target")?),
        ];
        match draw.get("position") {
            Some(_) => vals.extend(vec3_slots(draw, "position")?),
            None => vals.extend([None, None, None]),
        }
        rows.push(Eff::new("WATER_DRAW", vals));
    }
    for point in array(document, "event_points")? {
        rows.push(Eff::new(
            "FUNC_DATA",
            vec![Some(text_field(point, "type")?)],
        ));
        if let Some(name) = point.get("name") {
            rows.push(Eff::new("FUNC_NAME", vec![Some(text_val(name)?)]));
        }
        if let Some(event) = point.get("event") {
            rows.push(Eff::new(
                "FUNC_EVENT_DATA",
                vec![
                    Some(int_field(event, "point_no")?),
                    opt_field(event, "mode", text_val)?,
                ],
            ));
        }
        if let Some(t) = point.get("transform") {
            let mut vals = vec3_slots(t, "position")?;
            vals.extend(vec3_slots(t, "rotation")?);
            vals.extend(vec3_slots(t, "scale")?);
            rows.push(Eff::new("FUNC_POS", vals));
        }
    }
    Ok(())
}

fn flatten_places(
    table: Option<&Value>,
    start: &str,
    entry: &str,
    end: &str,
    rows: &mut Vec<Eff>,
) -> CoreResult<()> {
    let Some(table) = table else {
        return Ok(());
    };
    rows.push(Eff::new(start, vec![Some(int_field(table, "count")?)]));
    for part in array(table, "parts")? {
        let mut vals = vec![Some(int_field(part, "id")?)];
        vals.extend(vec3_slots(part, "position")?);
        vals.push(Some(int_field(part, "angle")?));
        rows.push(Eff::new(entry, vals));
    }
    rows.push(Eff::new(end, vec![]));
    Ok(())
}

fn flatten_georama(document: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    let Some(georama) = document.get("georama") else {
        return Ok(());
    };
    flatten_places(
        georama.get("fixed_parts"),
        "FIX_EPARTS_START",
        "FIX_EPARTS",
        "FIX_EPARTS_END",
        rows,
    )?;
    for river in array(georama, "river_parts")? {
        rows.push(Eff::new(
            "RIVER_PARTS_NAME",
            vec![
                Some(int_field(river, "index")?),
                Some(text_field(river, "parts")?),
                Some(text_field(river, "model")?),
            ],
        ));
    }
    for model in array(georama, "water_parts_models")? {
        rows.push(Eff::new("WATER_PARTS_NAME", vec![Some(text_val(model)?)]));
    }
    for mask in array(georama, "mask_parts")? {
        rows.push(Eff::new(
            "MASK_PARTS_NAME",
            vec![
                Some(int_field(mask, "index")?),
                Some(text_field(mask, "model")?),
            ],
        ));
    }
    flatten_places(
        georama.get("initial_parts"),
        "INIT_EPARTS_START",
        "INIT_EPARTS",
        "INIT_EPARTS_END",
        rows,
    )?;
    Ok(())
}

fn flatten_scene_config(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut cfg = Vec::new();
    flatten_cfg(document, &mut cfg)?;
    let mut georama = Vec::new();
    flatten_georama(document, &mut georama)?;
    Ok(vec![cfg, georama])
}

const SCENE_CONFIG: Family = Family {
    kind: "scene_config",
    passes: &[&CFG, &EMAP],
    // Retail scripts keep a FUNC_DATA_END the configuration table does not name, dungeon room
    // presets that only the dungeon generator's own files carry, and villager places that are read
    // from place.cfg instead.
    ignored: &[
        "FUNC_DATA_END",
        "VER",
        "ROOM_ID",
        "ROOM_SIZE",
        "ROOM_RATE",
        "ROOM_FIXED",
        "RD",
        "ROOM_END",
        "NPC_PLACE",
        "NPC_POS",
        "NPC_PLACE_END",
    ],
    build: build_scene_config,
    flatten: flatten_scene_config,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[&SCENE_CONFIG];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scene_configs_split_independent_tables_and_georama_parts() {
        let script = b"FIX_EPARTS_START 1; FIX_EPARTS 48,0,0,0,0; FIX_EPARTS_END; EDIT_RIVER \"r\";
            RIVER_PARTS_NAME 0,\"p\",\"r.gltf\"; WATER_PARTS_NAME \"w.gltf\"; EDIT_RIVER_END;
            FUNC_DATA \"event\"; FUNC_NAME \"e\"; FUNC_EVENT_DATA 201,\"every\"; FUNC_POS 0,0,0,0,0,0,1,1,1; FUNC_DATA_END;
            WATER_SURFACE_NUM 1; WATER_SURFACE_START; WATER_VERTEX 4,4,0,0,0,10,0,10; WATER_PARAM 1,2,3,4;
            WATER_SHAKE 1,2,0.5; WATER_SURFACE_END; WATER_DRAW_NUM 1; WATER_DRAW 0,\"#follow101\",0,-19,0;";
        let document = super::super::super::configs::parse(script).unwrap();
        let doc = super::super::build("map/g/g01/g01.cfg", &document)
            .unwrap()
            .0;
        assert_eq!(doc["kind"], "scene_config");
        assert_eq!(doc["water"]["surfaces"][0]["vertex"]["count_x"], 4);
        assert_eq!(
            doc["water"]["draws"][0]["position"],
            json!([0.0, -19.0, 0.0])
        );
        assert_eq!(doc["event_points"][0]["event"]["mode"], "every");
        assert_eq!(doc["georama"]["fixed_parts"]["parts"][0]["id"], 48);
        assert_eq!(doc["georama"]["water_parts_models"], json!(["w.gltf"]));
    }
}
