//! Menu screens: layout scripts read by `MenuDataAnalyze` and command scripts read by
//! `MenuCommandAnalyze` (menucommon.cpp).
use super::*;

// --- Shared helpers ---------------------------------------------------------------------------------

/// A read slot as a required integer.
fn req_i(value: &Value, key: &str) -> CoreResult<Option<Val>> {
    Ok(Some(int_field(value, key)?))
}

fn req_f(value: &Value, key: &str) -> CoreResult<Option<Val>> {
    Ok(Some(float_field(value, key)?))
}

fn req_s(value: &Value, key: &str) -> CoreResult<Option<Val>> {
    Ok(Some(text_field(value, key)?))
}

fn opt_i(value: &Value, key: &str) -> CoreResult<Option<Val>> {
    opt_field(value, key, int_val)
}

/// Every element of an integer array field as read slots.
fn int_list(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    array(value, key)?
        .iter()
        .map(|v| Ok(Some(int_val(v)?)))
        .collect()
}

fn float_list(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    array(value, key)?
        .iter()
        .map(|v| Ok(Some(float_val(v)?)))
        .collect()
}

/// Every read slot from `from` on, as integers.
fn rest_ints(eff: &Eff, from: usize) -> CoreResult<Value> {
    ints_json(eff, from, eff.count().saturating_sub(from))
}

fn rest_floats(eff: &Eff, from: usize) -> CoreResult<Value> {
    floats_json(eff, from, eff.count().saturating_sub(from))
}

fn text_at(eff: &Eff, slot: usize) -> CoreResult<Value> {
    Ok(text_json(&eff.text(slot)?))
}

/// The tag a command was written with, recorded only when it is not the usual spelling of its
/// callback.
fn spelling(obj: Obj, eff: &Eff, usual: &str) -> Obj {
    if eff.name == usual {
        obj
    } else {
        obj.put("spelling", json!(eff.name))
    }
}

/// The tag a command was written with, given the spellings its callback has.
fn spelled(value: &Value, usual: &'static str, others: &[&'static str]) -> CoreResult<String> {
    match value.get("spelling") {
        None => Ok(usual.into()),
        Some(Value::String(name)) if others.contains(&name.as_str()) => Ok(name.clone()),
        Some(other) => Err(fail(format!("{usual} cannot be spelled {other}"))),
    }
}

fn kind_of(value: &Value, key: &str) -> CoreResult<String> {
    field(value, key)?
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| fail(format!("field {key} is not a string")))
}

// --- menu_layout: MenuDataAnalyze (menu_analyze_tag) -------------------------------------------------

const LAYOUT: Pass = Pass {
    name: "menu_analyze_tag",
    commands: &[
        Command {
            name: "ETCINFO_MALLOC",
            layout: "i?",
        },
        Command {
            name: "ETCINFO_OFFSET",
            layout: "i",
        },
        Command {
            name: "ETCINFO",
            layout: "s i i",
        },
        Command {
            name: "ETCINFO_CLEAR",
            layout: "i i?",
        },
        Command {
            name: "ETCINFO2_MALLOC",
            layout: "i?",
        },
        Command {
            name: "ETCINFO2_OFFSET",
            layout: "i",
        },
        Command {
            name: "ETCINFO2",
            layout: "s *f",
        },
        Command {
            name: "ETCINFO2_CLEAR",
            layout: "i i?",
        },
        Command {
            name: "RESET_TEXINFO",
            layout: "",
        },
        Command {
            name: "INIT_DRAWLIST",
            layout: "",
        },
        Command {
            name: "TEXDATA_CLEAR",
            layout: "i i?",
        },
        Command {
            name: "TEXDATA_MALLOC",
            layout: "i?",
        },
        Command {
            name: "TEXNAME",
            layout: "s i",
        },
        Command {
            name: "TEXDATA_OFFSET",
            layout: "i",
        },
        Command {
            name: "TEXDATA",
            layout: "s i i i i",
        },
        Command {
            name: "TD",
            layout: "s i i i i",
        },
        Command {
            name: "FORM_CLEAR",
            layout: "*i",
        },
        Command {
            name: "FORM_MALLOC",
            layout: "i",
        },
        Command {
            name: "FORM_OFFSET_NO",
            layout: "i",
        },
        Command {
            name: "FORM_SET",
            layout: "s i?2",
        },
        Command {
            name: "FORM_PARTNUM",
            layout: "i",
        },
        Command {
            name: "FORM_DTYPE",
            layout: "s *v",
        },
        Command {
            name: "FORM_MTYPE",
            layout: "s",
        },
        Command {
            name: "FORM_DRAWFLG",
            layout: "*v",
        },
        Command {
            name: "FORM_VIBECNT",
            layout: "i i",
        },
        Command {
            name: "FORM_MOVERATE",
            layout: "f f",
        },
        Command {
            name: "FORM_PUTXY",
            layout: "i i",
        },
        Command {
            name: "FORM_RGBA",
            layout: "*i",
        },
        Command {
            name: "FORM_RGBABIT",
            layout: "s",
        },
        Command {
            name: "FORM_ACTTBL",
            layout: "i",
        },
        Command {
            name: "FORM_ACTDEF",
            layout: "s i f f f f",
        },
        Command {
            name: "FORM_SETACT",
            layout: "s",
        },
        Command {
            name: "PARTVIBECNT",
            layout: "i i",
        },
        Command {
            name: "PARTVIBER",
            layout: "i i",
        },
        Command {
            name: "SHADOW",
            layout: "*i",
        },
        Command {
            name: "CLIP",
            layout: "*i",
        },
        Command {
            name: "PARTRGBA",
            layout: "*i",
        },
        Command {
            name: "PARTALP_BLEND",
            layout: "i",
        },
        Command {
            name: "PART_ETCINFO",
            layout: "*i",
        },
        Command {
            name: "PART_BILINEAR",
            layout: "",
        },
        Command {
            name: "PART_DTYPE",
            layout: "s s *i",
        },
        Command {
            name: "NORMAL",
            layout: "s s i i i?4 i?4",
        },
        Command {
            name: "NRL",
            layout: "s s i i i?4 i?4",
        },
        Command {
            name: "NORMAL2",
            layout: "s s i i i?4 i?4",
        },
        Command {
            name: "CURSOR",
            layout: "s i i",
        },
        Command {
            name: "FUNCINFO",
            layout: "s i i",
        },
        Command {
            name: "NUMBER1",
            layout: "s s i i i i?5 i?5",
        },
        Command {
            name: "NUMBER2",
            layout: "s s i i i i?5 i?5",
        },
        Command {
            name: "FRMIMG",
            layout: "s s s i i i i",
        },
        Command {
            name: "FORM",
            layout: "i s i i",
        },
        Command {
            name: "FORM_SETEND",
            layout: "-",
        },
        Command {
            name: "ITEM",
            layout: "s s i i i?4 i?4",
        },
        Command {
            name: "ITEM_MARK",
            layout: "s i i i?3 i?3",
        },
        Command {
            name: "FILLBOX",
            layout: "s s i i i i i",
        },
        Command {
            name: "FILLBOXINFO",
            layout: "i i i i",
        },
        Command {
            name: "WAKU_RECT",
            layout: "s s i i i i",
        },
        Command {
            name: "WAKU_CIRCLE",
            layout: "s s i i i i",
        },
        Command {
            name: "PARTS_EFF_NUM",
            layout: "i",
        },
        Command {
            name: "PARTS_EFFECT",
            layout: "s *i",
        },
    ],
};

/// Commands that set an attribute of the current form.
const FORM_SETTINGS: &[&str] = &[
    "FORM_PARTNUM",
    "FORM_DTYPE",
    "FORM_MTYPE",
    "FORM_DRAWFLG",
    "FORM_VIBECNT",
    "FORM_MOVERATE",
    "FORM_PUTXY",
    "FORM_RGBA",
    "FORM_RGBABIT",
    "FORM_ACTTBL",
    "FORM_ACTDEF",
    "FORM_SETACT",
    "CLIP",
];

/// Commands that enter a new part into the current form.
const PART_KINDS: &[(&str, &str)] = &[
    ("NRL", "normal"),
    ("NORMAL", "normal"),
    ("NORMAL2", "normal2"),
    ("CURSOR", "cursor"),
    ("FUNCINFO", "funcinfo"),
    ("NUMBER1", "number1"),
    ("NUMBER2", "number2"),
    ("FRMIMG", "frame_image"),
    ("FORM", "form"),
    ("ITEM", "item"),
    ("ITEM_MARK", "item_checkmark"),
    ("FILLBOX", "fillbox"),
    ("WAKU_RECT", "waku_rect"),
    ("WAKU_CIRCLE", "waku_circle"),
    ("PART_DTYPE", "dtype"),
];

/// Commands that change the current part, or the effect records it allocated.
const PART_MODIFIERS: &[&str] = &[
    "SHADOW",
    "PARTRGBA",
    "PARTALP_BLEND",
    "PART_ETCINFO",
    "PART_BILINEAR",
    "PARTVIBECNT",
    "PARTVIBER",
    "PARTS_EFF_NUM",
    "PARTS_EFFECT",
    "FILLBOXINFO",
];

fn table_op(op: &str, count: Option<Value>) -> Value {
    Obj::new().put("op", json!(op)).opt("count", count).done()
}

fn range_op(op: &str, eff: &Eff) -> CoreResult<Value> {
    Ok(Obj::new()
        .put("op", json!(op))
        .put("from", json!(eff.int(0)?))
        .opt("to", slot_json(eff, 1))
        .done())
}

fn offset_op(op: &str, eff: &Eff) -> CoreResult<Value> {
    Ok(json!({"op": op, "index": eff.int(0)?}))
}

fn texture_json(eff: &Eff) -> CoreResult<Value> {
    Ok(spelling(
        Obj::new()
            .put("name", text_at(eff, 0)?)
            .put("rect", ints_json(eff, 1, 4)?),
        eff,
        "TD",
    )
    .done())
}

fn textures_json(cur: &mut Cursor) -> CoreResult<Vec<Value>> {
    let mut out = Vec::new();
    while matches!(cur.peek_name(), Some("TD" | "TEXDATA")) {
        out.push(texture_json(cur.next().unwrap())?);
    }
    Ok(out)
}

fn setting_json(cur: &mut Cursor) -> CoreResult<Value> {
    let row = cur.next().unwrap();
    let set = |name: &str| Obj::new().put("set", json!(name));
    Ok(match row.name.as_str() {
        "FORM_PARTNUM" => set("parts_capacity")
            .put("count", json!(row.int(0)?))
            .done(),
        "FORM_DTYPE" => {
            let kind = row.text(0)?;
            let mut obj = set("draw_type").put("type", text_json(&kind));
            let name = kind.as_ref().map(|t| t.text.as_str());
            let rest = &row.vals[1..];
            match (name, rest) {
                (_, []) => {}
                (Some("poly"), [_, _, _, _]) => {
                    if !rest.iter().all(|v| matches!(v, Some(Val::F(_)))) {
                        return Err(fail("FORM_DTYPE poly ambient is not four floats"));
                    }
                    obj = obj.put("ambient", floats_json(row, 1, 4)?);
                }
                (Some("geolist"), [Some(Val::I(list))]) => obj = obj.put("list", json!(list)),
                _ => return Err(fail(format!("FORM_DTYPE {name:?} with operands {rest:?}"))),
            }
            if name == Some("geolist") && rest.is_empty() {
                return Err(fail(
                    "FORM_DTYPE geolist without its list reads a stale slot",
                ));
            }
            obj.done()
        }
        "FORM_MTYPE" => set("move_type").put("type", text_at(row, 0)?).done(),
        "FORM_DRAWFLG" => match row.vals.as_slice() {
            [Some(Val::I(draw))] => set("draw_flag").put("draw", json!(draw)).done(),
            [Some(Val::S(form)), Some(Val::I(draw))] => set("other_form_draw_flag")
                .put("form", text_json(form))
                .put("draw", json!(draw))
                .done(),
            other => return Err(fail(format!("FORM_DRAWFLG with operands {other:?}"))),
        },
        "FORM_VIBECNT" => set("vibe_count")
            .put("counts", ints_json(row, 0, 2)?)
            .done(),
        "FORM_MOVERATE" => set("move_rate").put("rate", floats_json(row, 0, 2)?).done(),
        "FORM_PUTXY" => set("position").put("xy", ints_json(row, 0, 2)?).done(),
        "FORM_RGBA" => {
            if !matches!(row.count(), 3 | 4) {
                return Err(fail(format!("FORM_RGBA with {} operands", row.count())));
            }
            set("rgba").put("rgba", rest_ints(row, 0)?).done()
        }
        "FORM_RGBABIT" => set("rgba_bits").put("channels", text_at(row, 0)?).done(),
        "FORM_ACTTBL" => {
            let mut actions = Vec::new();
            while let Some(def) = cur.take("FORM_ACTDEF") {
                actions.push(json!({
                    "name": text_at(def, 0)?,
                    "move_type": def.int(1)?,
                    "xy": floats_json(def, 2, 2)?,
                    "rate": floats_json(def, 4, 2)?,
                }));
            }
            set("actions")
                .put("capacity", json!(row.int(0)?))
                .put("actions", Value::Array(actions))
                .done()
        }
        "FORM_ACTDEF" => return Err(fail("FORM_ACTDEF without FORM_ACTTBL before it")),
        "FORM_SETACT" => set("action").put("name", text_at(row, 0)?).done(),
        "CLIP" => match row.count() {
            0 => set("clip").done(),
            2 => set("clip").put("size", ints_json(row, 0, 2)?).done(),
            n => return Err(fail(format!("CLIP with {n} operands"))),
        },
        _ => unreachable!(),
    })
}

fn modifier_json(row: &Eff) -> CoreResult<Value> {
    let set = |name: &str| Obj::new().put("mod", json!(name));
    Ok(match row.name.as_str() {
        "SHADOW" => match row.count() {
            0 => set("shadow").done(),
            1 => set("shadow").put("on", json!(row.int(0)?)).done(),
            2 => set("shadow")
                .put("on", json!(row.int(0)?))
                .put("offset", json!(row.int(1)?))
                .done(),
            n => return Err(fail(format!("SHADOW with {n} operands"))),
        },
        "PARTRGBA" => match row.count() {
            1 => set("alpha").put("alpha", json!(row.int(0)?)).done(),
            3 | 4 => set("rgba").put("rgba", rest_ints(row, 0)?).done(),
            n => return Err(fail(format!("PARTRGBA with {n} operands"))),
        },
        "PARTALP_BLEND" => set("alpha_blend").put("mode", json!(row.int(0)?)).done(),
        "PART_ETCINFO" => {
            if !row.count().is_multiple_of(2) {
                return Err(fail("PART_ETCINFO with an odd operand count"));
            }
            let entries = (0..row.count() / 2)
                .map(|pair| {
                    Ok(json!({"index": row.int(pair * 2)?, "value": row.int(pair * 2 + 1)?}))
                })
                .collect::<CoreResult<Vec<_>>>()?;
            set("etc_info").put("entries", Value::Array(entries)).done()
        }
        "PART_BILINEAR" => set("bilinear").done(),
        "PARTVIBECNT" => set("vibe_count")
            .put("counts", ints_json(row, 0, 2)?)
            .done(),
        "PARTVIBER" => set("viber").put("ranges", ints_json(row, 0, 2)?).done(),
        "PARTS_EFF_NUM" => set("effect_capacity")
            .put("count", json!(row.int(0)?))
            .done(),
        "PARTS_EFFECT" => set("effect")
            .put("type", text_at(row, 0)?)
            .put("params", rest_ints(row, 1)?)
            .done(),
        "FILLBOXINFO" => set("fill").put("rgba", ints_json(row, 0, 4)?).done(),
        _ => unreachable!(),
    })
}

/// Width and height read only when the command has more than `guard` operands.
fn size_json(obj: Obj, row: &Eff, from: usize) -> Obj {
    match (slot_json(row, from), slot_json(row, from + 1)) {
        (Some(w), Some(h)) => obj.put("size", json!([w, h])),
        _ => obj,
    }
}

fn part_json(row: &Eff, kind: &str) -> CoreResult<Value> {
    let base = spelling(
        Obj::new().put("part", json!(kind)),
        row,
        if row.name == "NORMAL" {
            "NRL"
        } else {
            &row.name
        },
    );
    let xy = |from: usize| ints_json(row, from, 2);
    Ok(match row.name.as_str() {
        "NRL" | "NORMAL" | "NORMAL2" => size_json(
            base.put("texture", text_at(row, 0)?)
                .put("name", text_at(row, 1)?)
                .put("xy", xy(2)?),
            row,
            4,
        )
        .done(),
        "CURSOR" | "FUNCINFO" => base.put("name", text_at(row, 0)?).put("xy", xy(1)?).done(),
        "NUMBER1" | "NUMBER2" => size_json(
            base.put("texture", text_at(row, 0)?)
                .put("name", text_at(row, 1)?)
                .put("value", json!(row.int(2)?))
                .put("xy", xy(3)?),
            row,
            5,
        )
        .done(),
        "FRMIMG" => base
            .put("kind", text_at(row, 0)?)
            .put("texture", text_at(row, 1)?)
            .put("name", text_at(row, 2)?)
            .put("xy", xy(3)?)
            .put("size", xy(5)?)
            .done(),
        "FORM" => base
            .put("form", json!(row.int(0)?))
            .put("name", text_at(row, 1)?)
            .put("xy", xy(2)?)
            .done(),
        "ITEM" => size_json(
            base.put("kind", text_at(row, 0)?)
                .put("name", text_at(row, 1)?)
                .put("xy", xy(2)?),
            row,
            4,
        )
        .done(),
        "ITEM_MARK" => {
            size_json(base.put("name", text_at(row, 0)?).put("xy", xy(1)?), row, 3).done()
        }
        "FILLBOX" => base
            .put("kind", text_at(row, 0)?)
            .put("name", text_at(row, 1)?)
            .put("xy", xy(2)?)
            .put("size", xy(4)?)
            .put("style", json!(row.int(6)?))
            .done(),
        "WAKU_RECT" | "WAKU_CIRCLE" => base
            .put("texture", text_at(row, 0)?)
            .put("name", text_at(row, 1)?)
            .put("xy", xy(2)?)
            .put("size", xy(4)?)
            .done(),
        "PART_DTYPE" => {
            let kind = row.text(0)?;
            let name = kind.as_ref().map(|t| t.text.as_str());
            let obj = base
                .put("kind", text_json(&kind))
                .put("name", text_at(row, 1)?);
            match (name, row.count() - 2) {
                (Some("clut_reload"), _) => obj.put("etc_info", rest_ints(row, 2)?),
                (Some("\u{3054}\u{3061}\u{3083}\u{7dda}"), 4) => {
                    obj.put("xy", xy(2)?).put("size", xy(4)?)
                }
                (Some("\u{3054}\u{3061}\u{3083}\u{7dda}"), n) => {
                    return Err(fail(format!(
                        "PART_DTYPE random line with {n} values reads stale slots"
                    )));
                }
                (_, 0) => obj,
                (_, n) => return Err(fail(format!("PART_DTYPE {name:?} with {n} unread values"))),
            }
            .done()
        }
        _ => unreachable!(),
    })
}

fn form_json(cur: &mut Cursor) -> CoreResult<Value> {
    let set = cur.expect("FORM_SET")?;
    let mut settings = Vec::new();
    while cur
        .peek_name()
        .is_some_and(|name| FORM_SETTINGS.contains(&name))
    {
        settings.push(setting_json(cur)?);
    }
    if cur
        .peek_name()
        .is_some_and(|name| PART_MODIFIERS.contains(&name))
    {
        return Err(fail(
            "a part modifier follows FORM_SET before the form's first part",
        ));
    }
    let mut parts = Vec::new();
    while let Some(kind) = cur
        .peek_name()
        .and_then(|name| PART_KINDS.iter().find(|(tag, _)| *tag == name))
        .map(|(_, kind)| *kind)
    {
        let row = cur.next().unwrap();
        let mut part = part_json(row, kind)?;
        let mut modifiers = Vec::new();
        while cur
            .peek_name()
            .is_some_and(|name| PART_MODIFIERS.contains(&name))
        {
            modifiers.push(modifier_json(cur.next().unwrap())?);
        }
        if !modifiers.is_empty() {
            part["modifiers"] = Value::Array(modifiers);
        }
        parts.push(part);
    }
    if cur
        .peek_name()
        .is_some_and(|name| FORM_SETTINGS.contains(&name))
    {
        return Err(fail("a form setting follows the form's parts"));
    }
    Ok(Obj::new()
        .put("op", json!("form"))
        .put("name", text_at(set, 0)?)
        .opt("slot", slot_json(set, 1))
        .put("settings", Value::Array(settings))
        .put("parts", Value::Array(parts))
        .done())
}

fn build_layout(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cur = Cursor::new(&passes[0]);
    let mut ops = Vec::new();
    while let Some(row) = cur.peek() {
        let name = row.name.as_str();
        if name == "FORM_SET" {
            ops.push(form_json(&mut cur)?);
            continue;
        }
        if name == "TEXNAME" {
            cur.next();
            ops.push(json!({
                "op": "texture_sheet",
                "sheet": text_at(row, 0)?,
                "block_slot": row.int(1)?,
                "textures": textures_json(&mut cur)?,
            }));
            continue;
        }
        if name == "TD" || name == "TEXDATA" {
            ops.push(json!({"op": "textures", "textures": textures_json(&mut cur)?}));
            continue;
        }
        if name == "ETCINFO" || name == "ETCINFO2" {
            let mut entries = Vec::new();
            while let Some(entry) = cur.take(name) {
                let values = if name == "ETCINFO" {
                    ints_json(entry, 1, 2)?
                } else {
                    rest_floats(entry, 1)?
                };
                entries.push(json!({"name": text_at(entry, 0)?, "values": values}));
            }
            let op = if name == "ETCINFO" {
                "etc_info"
            } else {
                "etc_info2"
            };
            ops.push(json!({"op": op, "entries": entries}));
            continue;
        }
        cur.next();
        ops.push(match name {
            "ETCINFO_MALLOC" => table_op("allocate_etc_info", slot_json(row, 0)),
            "ETCINFO2_MALLOC" => table_op("allocate_etc_info2", slot_json(row, 0)),
            "TEXDATA_MALLOC" => table_op("allocate_textures", slot_json(row, 0)),
            "FORM_MALLOC" => table_op("allocate_forms", slot_json(row, 0)),
            "ETCINFO_CLEAR" => range_op("clear_etc_info", row)?,
            "ETCINFO2_CLEAR" => range_op("clear_etc_info2", row)?,
            "TEXDATA_CLEAR" => range_op("clear_textures", row)?,
            "FORM_CLEAR" => match row.count() {
                0 => json!({"op": "clear_forms"}),
                2 => json!({"op": "clear_forms", "from": row.int(0)?, "to": row.int(1)?}),
                n => return Err(fail(format!("FORM_CLEAR with {n} operands"))),
            },
            "ETCINFO_OFFSET" => offset_op("etc_info_offset", row)?,
            "ETCINFO2_OFFSET" => offset_op("etc_info2_offset", row)?,
            "TEXDATA_OFFSET" => offset_op("texture_offset", row)?,
            "FORM_OFFSET_NO" => offset_op("form_offset", row)?,
            "INIT_DRAWLIST" => json!({"op": "init_drawlist"}),
            "RESET_TEXINFO" => json!({"op": "reset_texinfo"}),
            _ => {
                return Err(fail(format!(
                    "{name} at row {} has no form or part of this script to apply to",
                    cur_index(&cur)
                )));
            }
        });
    }
    Ok(json!({ "operations": ops }))
}

fn cur_index(cur: &Cursor) -> String {
    cur.peek_name().unwrap_or("end").to_owned()
}

fn flatten_texture(value: &Value) -> CoreResult<Eff> {
    let mut vals = vec![req_s(value, "name")?];
    vals.extend(int_slots(value, "rect", 4)?);
    Ok(Eff::new(&spelled(value, "TD", &["TEXDATA"])?, vals))
}

fn flatten_setting(value: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    let row = |name: &str, vals: Vec<Option<Val>>| Eff::new(name, vals);
    match kind_of(value, "set")?.as_str() {
        "parts_capacity" => rows.push(row("FORM_PARTNUM", vec![req_i(value, "count")?])),
        "draw_type" => {
            let mut vals = vec![req_s(value, "type")?];
            if value.get("ambient").is_some() {
                vals.extend(float_slots(value, "ambient", 4)?);
            }
            if let Some(list) = opt_i(value, "list")? {
                vals.push(Some(list));
            }
            rows.push(row("FORM_DTYPE", vals));
        }
        "move_type" => rows.push(row("FORM_MTYPE", vec![req_s(value, "type")?])),
        "draw_flag" => rows.push(row("FORM_DRAWFLG", vec![req_i(value, "draw")?])),
        "other_form_draw_flag" => rows.push(row(
            "FORM_DRAWFLG",
            vec![req_s(value, "form")?, req_i(value, "draw")?],
        )),
        "vibe_count" => rows.push(row("FORM_VIBECNT", int_slots(value, "counts", 2)?)),
        "move_rate" => rows.push(row("FORM_MOVERATE", float_slots(value, "rate", 2)?)),
        "position" => rows.push(row("FORM_PUTXY", int_slots(value, "xy", 2)?)),
        "rgba" => rows.push(row("FORM_RGBA", int_list(value, "rgba")?)),
        "rgba_bits" => rows.push(row("FORM_RGBABIT", vec![req_s(value, "channels")?])),
        "actions" => {
            rows.push(row("FORM_ACTTBL", vec![req_i(value, "capacity")?]));
            for action in array(value, "actions")? {
                let mut vals = vec![req_s(action, "name")?, req_i(action, "move_type")?];
                vals.extend(float_slots(action, "xy", 2)?);
                vals.extend(float_slots(action, "rate", 2)?);
                rows.push(row("FORM_ACTDEF", vals));
            }
        }
        "action" => rows.push(row("FORM_SETACT", vec![req_s(value, "name")?])),
        "clip" => rows.push(row(
            "CLIP",
            if value.get("size").is_some() {
                int_slots(value, "size", 2)?
            } else {
                Vec::new()
            },
        )),
        other => return Err(fail(format!("unknown form setting {other}"))),
    }
    Ok(())
}

fn flatten_modifier(value: &Value) -> CoreResult<Eff> {
    let (name, vals) = match kind_of(value, "mod")?.as_str() {
        "shadow" => {
            let mut vals = Vec::new();
            if let Some(on) = opt_i(value, "on")? {
                vals.push(Some(on));
                if let Some(offset) = opt_i(value, "offset")? {
                    vals.push(Some(offset));
                }
            }
            ("SHADOW", vals)
        }
        "alpha" => ("PARTRGBA", vec![req_i(value, "alpha")?]),
        "rgba" => ("PARTRGBA", int_list(value, "rgba")?),
        "alpha_blend" => ("PARTALP_BLEND", vec![req_i(value, "mode")?]),
        "etc_info" => {
            let mut vals = Vec::new();
            for entry in array(value, "entries")? {
                vals.push(req_i(entry, "index")?);
                vals.push(req_i(entry, "value")?);
            }
            ("PART_ETCINFO", vals)
        }
        "bilinear" => ("PART_BILINEAR", Vec::new()),
        "vibe_count" => ("PARTVIBECNT", int_slots(value, "counts", 2)?),
        "viber" => ("PARTVIBER", int_slots(value, "ranges", 2)?),
        "effect_capacity" => ("PARTS_EFF_NUM", vec![req_i(value, "count")?]),
        "effect" => {
            let mut vals = vec![req_s(value, "type")?];
            vals.extend(int_list(value, "params")?);
            ("PARTS_EFFECT", vals)
        }
        "fill" => ("FILLBOXINFO", int_slots(value, "rgba", 4)?),
        other => return Err(fail(format!("unknown part modifier {other}"))),
    };
    Ok(Eff::new(name, vals))
}

/// Width and height slots that are present only when the part gives a size.
fn size_slots(value: &Value) -> CoreResult<Vec<Option<Val>>> {
    if value.get("size").is_some() {
        int_slots(value, "size", 2)
    } else {
        Ok(vec![None, None])
    }
}

fn flatten_part(value: &Value) -> CoreResult<Eff> {
    let kind = kind_of(value, "part")?;
    let xy = || int_slots(value, "xy", 2);
    let size = || int_slots(value, "size", 2);
    let (name, vals): (String, Vec<Option<Val>>) = match kind.as_str() {
        "normal" | "normal2" => {
            let tag = if kind == "normal" {
                spelled(value, "NRL", &["NORMAL"])?
            } else {
                "NORMAL2".into()
            };
            let mut vals = vec![req_s(value, "texture")?, req_s(value, "name")?];
            vals.extend(xy()?);
            vals.extend(size_slots(value)?);
            (tag, vals)
        }
        "cursor" | "funcinfo" => {
            let mut vals = vec![req_s(value, "name")?];
            vals.extend(xy()?);
            (kind.to_ascii_uppercase(), vals)
        }
        "number1" | "number2" => {
            let mut vals = vec![
                req_s(value, "texture")?,
                req_s(value, "name")?,
                req_i(value, "value")?,
            ];
            vals.extend(xy()?);
            vals.extend(size_slots(value)?);
            (kind.to_ascii_uppercase(), vals)
        }
        "frame_image" => {
            let mut vals = vec![
                req_s(value, "kind")?,
                req_s(value, "texture")?,
                req_s(value, "name")?,
            ];
            vals.extend(xy()?);
            vals.extend(size()?);
            ("FRMIMG".into(), vals)
        }
        "form" => {
            let mut vals = vec![req_i(value, "form")?, req_s(value, "name")?];
            vals.extend(xy()?);
            ("FORM".into(), vals)
        }
        "item" => {
            let mut vals = vec![req_s(value, "kind")?, req_s(value, "name")?];
            vals.extend(xy()?);
            vals.extend(size_slots(value)?);
            ("ITEM".into(), vals)
        }
        "item_checkmark" => {
            let mut vals = vec![req_s(value, "name")?];
            vals.extend(xy()?);
            vals.extend(size_slots(value)?);
            ("ITEM_MARK".into(), vals)
        }
        "fillbox" => {
            let mut vals = vec![req_s(value, "kind")?, req_s(value, "name")?];
            vals.extend(xy()?);
            vals.extend(size()?);
            vals.push(req_i(value, "style")?);
            ("FILLBOX".into(), vals)
        }
        "waku_rect" | "waku_circle" => {
            let mut vals = vec![req_s(value, "texture")?, req_s(value, "name")?];
            vals.extend(xy()?);
            vals.extend(size()?);
            (kind.to_ascii_uppercase(), vals)
        }
        "dtype" => {
            let mut vals = vec![req_s(value, "kind")?, req_s(value, "name")?];
            if value.get("etc_info").is_some() {
                vals.extend(int_list(value, "etc_info")?);
            }
            if value.get("xy").is_some() {
                vals.extend(xy()?);
                vals.extend(size()?);
            }
            ("PART_DTYPE".into(), vals)
        }
        other => return Err(fail(format!("unknown part {other}"))),
    };
    Ok(Eff { name, vals })
}

fn flatten_layout(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for op in array(document, "operations")? {
        let one = |name: &str, vals: Vec<Option<Val>>| Eff::new(name, vals);
        match kind_of(op, "op")?.as_str() {
            "allocate_etc_info" => rows.push(one("ETCINFO_MALLOC", vec![opt_i(op, "count")?])),
            "allocate_etc_info2" => rows.push(one("ETCINFO2_MALLOC", vec![opt_i(op, "count")?])),
            "allocate_textures" => rows.push(one("TEXDATA_MALLOC", vec![opt_i(op, "count")?])),
            "allocate_forms" => rows.push(one("FORM_MALLOC", vec![req_i(op, "count")?])),
            "clear_etc_info" => rows.push(one(
                "ETCINFO_CLEAR",
                vec![req_i(op, "from")?, opt_i(op, "to")?],
            )),
            "clear_etc_info2" => rows.push(one(
                "ETCINFO2_CLEAR",
                vec![req_i(op, "from")?, opt_i(op, "to")?],
            )),
            "clear_textures" => rows.push(one(
                "TEXDATA_CLEAR",
                vec![req_i(op, "from")?, opt_i(op, "to")?],
            )),
            "clear_forms" => rows.push(one(
                "FORM_CLEAR",
                if op.get("from").is_some() {
                    vec![req_i(op, "from")?, req_i(op, "to")?]
                } else {
                    Vec::new()
                },
            )),
            "etc_info_offset" => rows.push(one("ETCINFO_OFFSET", vec![req_i(op, "index")?])),
            "etc_info2_offset" => rows.push(one("ETCINFO2_OFFSET", vec![req_i(op, "index")?])),
            "texture_offset" => rows.push(one("TEXDATA_OFFSET", vec![req_i(op, "index")?])),
            "form_offset" => rows.push(one("FORM_OFFSET_NO", vec![req_i(op, "index")?])),
            "init_drawlist" => rows.push(one("INIT_DRAWLIST", Vec::new())),
            "reset_texinfo" => rows.push(one("RESET_TEXINFO", Vec::new())),
            "etc_info" => {
                for entry in array(op, "entries")? {
                    let mut vals = vec![req_s(entry, "name")?];
                    vals.extend(int_slots(entry, "values", 2)?);
                    rows.push(one("ETCINFO", vals));
                }
            }
            "etc_info2" => {
                for entry in array(op, "entries")? {
                    let mut vals = vec![req_s(entry, "name")?];
                    vals.extend(float_list(entry, "values")?);
                    rows.push(one("ETCINFO2", vals));
                }
            }
            "texture_sheet" => {
                rows.push(one(
                    "TEXNAME",
                    vec![req_s(op, "sheet")?, req_i(op, "block_slot")?],
                ));
                for texture in array(op, "textures")? {
                    rows.push(flatten_texture(texture)?);
                }
            }
            "textures" => {
                for texture in array(op, "textures")? {
                    rows.push(flatten_texture(texture)?);
                }
            }
            "form" => {
                rows.push(one(
                    "FORM_SET",
                    vec![req_s(op, "name")?, opt_i(op, "slot")?],
                ));
                for setting in array(op, "settings")? {
                    flatten_setting(setting, &mut rows)?;
                }
                for part in array(op, "parts")? {
                    rows.push(flatten_part(part)?);
                    for modifier in part
                        .get("modifiers")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                    {
                        rows.push(flatten_modifier(modifier)?);
                    }
                }
            }
            other => return Err(fail(format!("unknown layout operation {other}"))),
        }
    }
    Ok(vec![rows])
}

const MENU_LAYOUT: Family = Family {
    kind: "menu_layout",
    passes: &[&LAYOUT],
    ignored: &["FORM_PUXY", "FORM_ACTSET"],
    build: build_layout,
    flatten: flatten_layout,
    ordered: true,
};

// --- menu_commands: MenuCommandAnalyze (menu_execommand_analyze_tag) ---------------------------------

const COMMANDS: Pass = Pass {
    name: "menu_execommand_analyze_tag",
    commands: &[
        Command {
            name: "EXE_NAME",
            layout: "s",
        },
        Command {
            name: "FRM_DRAWFLG",
            layout: "i *s",
        },
        Command {
            name: "FRM_RGBA",
            layout: "s i?1 i?1 i?1 i?1",
        },
        Command {
            name: "FRM_CALCRGBA",
            layout: "s i i i",
        },
        Command {
            name: "FRM_FADE",
            layout: "s i i",
        },
        Command {
            name: "FRM_SETPOS",
            layout: "s i i i?3",
        },
        Command {
            name: "FRM_SETACTION",
            layout: "s s",
        },
        Command {
            name: "FRM_PARTSONOFF",
            layout: "s s i",
        },
        Command {
            name: "FRM_PARTSONOFFG",
            layout: "s i *s",
        },
        Command {
            name: "FRM_PTSONOFF",
            layout: "s s i",
        },
        Command {
            name: "FRM_PTSONOFFG",
            layout: "s i *s",
        },
        Command {
            name: "FRM_SWAP",
            layout: "s s",
        },
        Command {
            name: "FRM_SWAP_G",
            layout: "s s s s",
        },
        Command {
            name: "INIT_DRAWLIST",
            layout: "",
        },
        Command {
            name: "RESET_TEXINFO",
            layout: "",
        },
        Command {
            name: "MSG_ENV",
            layout: "i s",
        },
        Command {
            name: "MSG_MAKEMSG",
            layout: "i i",
        },
        Command {
            name: "MSG_ABSPOS",
            layout: "i i",
        },
        Command {
            name: "MSG_SYSBUF",
            layout: "i i",
        },
        Command {
            name: "MSG_BUF",
            layout: "i i",
        },
        Command {
            name: "MSG_FUCHI",
            layout: "i s",
        },
        Command {
            name: "MSG_SETCUR",
            layout: "i i",
        },
        Command {
            name: "MSG_SETGYOU",
            layout: "i i",
        },
        Command {
            name: "MSG_SPD",
            layout: "i f",
        },
        Command {
            name: "IN_KEY",
            layout: "i",
        },
        Command {
            name: "CURSOR_ONOFF",
            layout: "i",
        },
        Command {
            name: "CURSOR_FADE",
            layout: "i *i",
        },
        Command {
            name: "SET_WAKU",
            layout: "i",
        },
        Command {
            name: "SCN_FADE",
            layout: "i *i",
        },
        Command {
            name: "SE_ON",
            layout: "s",
        },
        Command {
            name: "PRINT",
            layout: "s",
        },
    ],
};

/// Message-window commands that take a window and one integer.
const MESSAGE_INTS: &[(&str, &str, &str)] = &[
    ("MSG_MAKEMSG", "make_message", "message"),
    ("MSG_ABSPOS", "message_abs_pos", "value"),
    ("MSG_SYSBUF", "message_system_buffer", "buffer"),
    ("MSG_BUF", "message_buffer", "buffer"),
    ("MSG_SETCUR", "message_cursor", "cursor"),
    ("MSG_SETGYOU", "message_question_row", "row"),
];

fn command_json(row: &Eff) -> CoreResult<Value> {
    let op = |name: &str| Obj::new().put("op", json!(name));
    if let Some((_, name, key)) = MESSAGE_INTS.iter().find(|(tag, _, _)| *tag == row.name) {
        return Ok(op(name)
            .put("window", json!(row.int(0)?))
            .put(key, json!(row.int(1)?))
            .done());
    }
    let names = |from: usize| -> CoreResult<Value> {
        (from..row.count())
            .map(|slot| text_at(row, slot))
            .collect::<CoreResult<Vec<_>>>()
            .map(Value::Array)
    };
    Ok(match row.name.as_str() {
        "FRM_DRAWFLG" => op("form_draw_flag")
            .put("draw", json!(row.int(0)?))
            .put("forms", names(1)?)
            .done(),
        "FRM_RGBA" => {
            let obj = op("form_rgba").put("form", text_at(row, 0)?);
            if row.has(1) {
                obj.put("rgba", ints_json(row, 1, 4)?)
            } else {
                obj
            }
            .done()
        }
        "FRM_CALCRGBA" => op("form_calc_rgba")
            .put("form", text_at(row, 0)?)
            .put("channel", json!(row.int(1)?))
            .put("step", json!(row.int(2)?))
            .put("target", json!(row.int(3)?))
            .done(),
        "FRM_FADE" => op("form_fade")
            .put("form", text_at(row, 0)?)
            .put("mode", json!(row.int(1)?))
            .put("frames", json!(row.int(2)?))
            .done(),
        "FRM_SETPOS" => op("form_set_pos")
            .put("form", text_at(row, 0)?)
            .put("xy", ints_json(row, 1, 2)?)
            .opt("language", slot_json(row, 3))
            .done(),
        "FRM_SETACTION" => op("form_set_action")
            .put("form", text_at(row, 0)?)
            .put("action", text_at(row, 1)?)
            .done(),
        "FRM_PARTSONOFF" | "FRM_PTSONOFF" => spelling(
            op("part_draw_flag")
                .put("form", text_at(row, 0)?)
                .put("part", text_at(row, 1)?)
                .put("draw", json!(row.int(2)?)),
            row,
            "FRM_PARTSONOFF",
        )
        .done(),
        "FRM_PARTSONOFFG" | "FRM_PTSONOFFG" => spelling(
            op("parts_draw_flag")
                .put("form", text_at(row, 0)?)
                .put("draw", json!(row.int(1)?))
                .put("parts", names(2)?),
            row,
            "FRM_PTSONOFFG",
        )
        .done(),
        "FRM_SWAP" => op("form_swap")
            .put("forms", json!([text_at(row, 0)?, text_at(row, 1)?]))
            .done(),
        "FRM_SWAP_G" => op("form_group_swap").put("forms", names(0)?).done(),
        "INIT_DRAWLIST" => op("init_drawlist").done(),
        "RESET_TEXINFO" => op("reset_texinfo").done(),
        "MSG_ENV" => op("message_env")
            .put("window", json!(row.int(0)?))
            .put("preset", text_at(row, 1)?)
            .done(),
        "MSG_FUCHI" => op("message_outline")
            .put("window", json!(row.int(0)?))
            .put("style", text_at(row, 1)?)
            .done(),
        "MSG_SPD" => op("message_speed")
            .put("window", json!(row.int(0)?))
            .put("speed", float_json(row.float(1)?))
            .done(),
        "IN_KEY" => op("input_key").put("enable", json!(row.int(0)?)).done(),
        "CURSOR_ONOFF" => op("cursor_on_off").put("show", json!(row.int(0)?)).done(),
        "CURSOR_FADE" => {
            let obj = op("cursor_fade").put("fade_in", json!(row.int(0)?));
            match row.count() {
                1 => obj,
                2 => obj.put("speed", json!(row.int(1)?)),
                3 => obj
                    .put("speed", json!(row.int(1)?))
                    .put("steps", json!(row.int(2)?)),
                n => return Err(fail(format!("CURSOR_FADE with {n} operands"))),
            }
            .done()
        }
        "SET_WAKU" => op("frame_type").put("type", json!(row.int(0)?)).done(),
        "SCN_FADE" => {
            let obj = op("scene_fade").put("fade_in", json!(row.int(0)?));
            match row.count() {
                1 => obj,
                2 => obj.put("frames", json!(row.int(1)?)),
                n => return Err(fail(format!("SCN_FADE with {n} operands"))),
            }
            .done()
        }
        "SE_ON" => op("play_se").put("name", text_at(row, 0)?).done(),
        "PRINT" => op("print").put("text", text_at(row, 0)?).done(),
        other => return Err(fail(format!("{other} is not a menu command"))),
    })
}

fn build_commands(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut blocks: Vec<Value> = Vec::new();
    for row in &passes[0] {
        if row.name == "EXE_NAME" {
            blocks.push(json!({"name": text_at(row, 0)?, "commands": []}));
            continue;
        }
        let Some(block) = blocks.last_mut() else {
            return Err(fail(format!(
                "{} before the first EXE_NAME runs only after another script's matching block",
                row.name
            )));
        };
        block["commands"]
            .as_array_mut()
            .unwrap()
            .push(command_json(row)?);
    }
    Ok(json!({ "blocks": blocks }))
}

fn flatten_command(value: &Value) -> CoreResult<Eff> {
    let op = kind_of(value, "op")?;
    if let Some((tag, _, key)) = MESSAGE_INTS.iter().find(|(_, name, _)| *name == op) {
        return Ok(Eff::new(
            tag,
            vec![req_i(value, "window")?, req_i(value, key)?],
        ));
    }
    let texts = |key: &str| -> CoreResult<Vec<Option<Val>>> {
        array(value, key)?
            .iter()
            .map(|v| Ok(Some(text_val(v)?)))
            .collect()
    };
    let (name, vals): (String, Vec<Option<Val>>) = match op.as_str() {
        "form_draw_flag" => {
            let mut vals = vec![req_i(value, "draw")?];
            vals.extend(texts("forms")?);
            ("FRM_DRAWFLG".into(), vals)
        }
        "form_rgba" => {
            let mut vals = vec![req_s(value, "form")?];
            if value.get("rgba").is_some() {
                vals.extend(int_slots(value, "rgba", 4)?);
            } else {
                vals.extend([None, None, None, None]);
            }
            ("FRM_RGBA".into(), vals)
        }
        "form_calc_rgba" => (
            "FRM_CALCRGBA".into(),
            vec![
                req_s(value, "form")?,
                req_i(value, "channel")?,
                req_i(value, "step")?,
                req_i(value, "target")?,
            ],
        ),
        "form_fade" => (
            "FRM_FADE".into(),
            vec![
                req_s(value, "form")?,
                req_i(value, "mode")?,
                req_i(value, "frames")?,
            ],
        ),
        "form_set_pos" => {
            let mut vals = vec![req_s(value, "form")?];
            vals.extend(int_slots(value, "xy", 2)?);
            vals.push(opt_i(value, "language")?);
            ("FRM_SETPOS".into(), vals)
        }
        "form_set_action" => (
            "FRM_SETACTION".into(),
            vec![req_s(value, "form")?, req_s(value, "action")?],
        ),
        "part_draw_flag" => (
            spelled(value, "FRM_PARTSONOFF", &["FRM_PTSONOFF"])?,
            vec![
                req_s(value, "form")?,
                req_s(value, "part")?,
                req_i(value, "draw")?,
            ],
        ),
        "parts_draw_flag" => {
            let mut vals = vec![req_s(value, "form")?, req_i(value, "draw")?];
            vals.extend(texts("parts")?);
            (spelled(value, "FRM_PTSONOFFG", &["FRM_PARTSONOFFG"])?, vals)
        }
        "form_swap" => {
            let vals = texts("forms")?;
            if vals.len() != 2 {
                return Err(fail("form_swap needs two forms"));
            }
            ("FRM_SWAP".into(), vals)
        }
        "form_group_swap" => {
            let vals = texts("forms")?;
            if vals.len() != 4 {
                return Err(fail("form_group_swap needs four forms"));
            }
            ("FRM_SWAP_G".into(), vals)
        }
        "init_drawlist" => ("INIT_DRAWLIST".into(), Vec::new()),
        "reset_texinfo" => ("RESET_TEXINFO".into(), Vec::new()),
        "message_env" => (
            "MSG_ENV".into(),
            vec![req_i(value, "window")?, req_s(value, "preset")?],
        ),
        "message_outline" => (
            "MSG_FUCHI".into(),
            vec![req_i(value, "window")?, req_s(value, "style")?],
        ),
        "message_speed" => (
            "MSG_SPD".into(),
            vec![req_i(value, "window")?, req_f(value, "speed")?],
        ),
        "input_key" => ("IN_KEY".into(), vec![req_i(value, "enable")?]),
        "cursor_on_off" => ("CURSOR_ONOFF".into(), vec![req_i(value, "show")?]),
        "cursor_fade" => {
            let mut vals = vec![req_i(value, "fade_in")?];
            if let Some(speed) = opt_i(value, "speed")? {
                vals.push(Some(speed));
                if let Some(steps) = opt_i(value, "steps")? {
                    vals.push(Some(steps));
                }
            }
            ("CURSOR_FADE".into(), vals)
        }
        "frame_type" => ("SET_WAKU".into(), vec![req_i(value, "type")?]),
        "scene_fade" => {
            let mut vals = vec![req_i(value, "fade_in")?];
            if let Some(frames) = opt_i(value, "frames")? {
                vals.push(Some(frames));
            }
            ("SCN_FADE".into(), vals)
        }
        "play_se" => ("SE_ON".into(), vec![req_s(value, "name")?]),
        "print" => ("PRINT".into(), vec![req_s(value, "text")?]),
        other => return Err(fail(format!("unknown menu command {other}"))),
    };
    Ok(Eff { name, vals })
}

fn flatten_commands(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for block in array(document, "blocks")? {
        rows.push(Eff::new("EXE_NAME", vec![req_s(block, "name")?]));
        for command in array(block, "commands")? {
            rows.push(flatten_command(command)?);
        }
    }
    Ok(vec![rows])
}

const MENU_COMMANDS: Family = Family {
    kind: "menu_commands",
    passes: &[&COMMANDS],
    ignored: &[
        "FRM_DRAFLG",
        "FMR_DRAWFLG",
        "CURSOR_ONFF",
        "SE_OK",
        "FRM_CALCRGBA\u{3000}\"msg7\",3,8,128",
    ],
    build: build_commands,
    flatten: flatten_commands,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[&MENU_LAYOUT, &MENU_COMMANDS];

/// Layout scripts a menu screen gives to `MenuDataAnalyze`, by their name inside the screen's pack.
const LAYOUT_SCRIPTS: &[&str] = &[
    "chrchg.cfg",
    "geo.cfg",
    "invent.cfg",
    "item.cfg",
    "manual.cfg",
    "mosbox.cfg",
    "npcmove.cfg",
    "option.cfg",
    "save.cfg",
    "shop.cfg",
    "trush.cfg",
    "wmap.cfg",
];

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    // menu/<language>/<screen>.pac/<script>.cfg
    let mut parts = path.split('/');
    let (Some("menu"), Some(language), Some(pack), Some(_), None) = (
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
    ) else {
        return None;
    };
    if language.is_empty()
        || !language.bytes().all(|b| b.is_ascii_digit())
        || !pack.ends_with(".pac")
    {
        return None;
    }
    if leaf.ends_with("_com.cfg") || leaf == "omake_gyo.cfg" {
        return Some("menu_commands");
    }
    let menu_main = leaf
        .strip_prefix("menu")
        .and_then(|rest| rest.strip_suffix(".cfg"))
        .is_some_and(|digits| !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit()));
    if menu_main || LAYOUT_SCRIPTS.contains(&leaf) {
        return Some("menu_layout");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> Value {
        let document = super::super::super::configs::parse(script).unwrap();
        super::super::build(alias, &document).unwrap().0
    }

    fn refuse(alias: &str, script: &[u8]) -> String {
        let document = super::super::super::configs::parse(script).unwrap();
        super::super::build(alias, &document)
            .unwrap_err()
            .to_string()
    }

    const LAYOUT_ALIAS: &str = "menu/1/men0.pac/menu0.cfg";
    const COMMAND_ALIAS: &str = "menu/1/itemmn0.pac/item_com.cfg";

    #[test]
    fn routes_menu_scripts() {
        assert_eq!(
            kind("menu/1/men0.pac/menu0.cfg", "menu0.cfg"),
            Some("menu_layout")
        );
        assert_eq!(
            kind("menu/0/wmap2.pac/wmap.cfg", "wmap.cfg"),
            Some("menu_layout")
        );
        assert_eq!(
            kind("menu/1/dmap0.pac/dtmap_com.cfg", "dtmap_com.cfg"),
            Some("menu_commands")
        );
        assert_eq!(
            kind("menu/1/omake.pac/omake_gyo.cfg", "omake_gyo.cfg"),
            Some("menu_commands")
        );
        assert_eq!(kind("menu/1/dmap0.pac/dmap0.cfg", "dmap0.cfg"), None);
        assert_eq!(kind("menu/1/wldmap.cfg", "wldmap.cfg"), None);
        assert_eq!(kind("menu/1/shop.pac/shoplst.cfg", "shoplst.cfg"), None);
    }

    #[test]
    fn layout_tables_textures_and_cursors() {
        let doc = convert(
            LAYOUT_ALIAS,
            b"ETCINFO_MALLOC 96; TEXDATA_MALLOC; TEXDATA_CLEAR 0,255; ETCINFO_CLEAR 3;\
              ETCINFO_OFFSET 4; ETCINFO \"a\",1,2; ETCINFO \"b\",3,4; ETCINFO2 \"c\",1.5,2;\
              TEXDATA_OFFSET 0; TEXNAME \"mn\",1; TD \"t0\",1,2,3,4; TEXDATA \"t1\",5,6,7,8;\
              FORM_MALLOC 10; FORM_CLEAR 0,10; FORM_OFFSET_NO 80; INIT_DRAWLIST; RESET_TEXINFO;",
        );
        let ops = doc["operations"].as_array().unwrap();
        assert_eq!(ops[0], json!({"op": "allocate_etc_info", "count": 96}));
        assert_eq!(ops[1], json!({"op": "allocate_textures"}));
        assert_eq!(
            ops[2],
            json!({"op": "clear_textures", "from": 0, "to": 255})
        );
        assert_eq!(ops[3], json!({"op": "clear_etc_info", "from": 3}));
        assert_eq!(ops[4], json!({"op": "etc_info_offset", "index": 4}));
        assert_eq!(ops[5]["entries"][1], json!({"name": "b", "values": [3, 4]}));
        assert_eq!(ops[6]["entries"][0]["values"], json!([1.5, 2.0]));
        assert_eq!(ops[8]["sheet"], "mn");
        assert_eq!(
            ops[8]["textures"][0],
            json!({"name": "t0", "rect": [1, 2, 3, 4]})
        );
        assert_eq!(ops[8]["textures"][1]["spelling"], "TEXDATA");
        assert_eq!(ops[10], json!({"op": "clear_forms", "from": 0, "to": 10}));
        assert_eq!(ops[11], json!({"op": "form_offset", "index": 80}));
    }

    #[test]
    fn layout_forms_nest_settings_parts_and_modifiers() {
        let doc = convert(
            LAYOUT_ALIAS,
            b"FORM_SET \"f\",3,0; FORM_PARTNUM 3; FORM_DRAWFLG 0; FORM_DRAWFLG \"g\",1;\
              FORM_ACTTBL 1; FORM_ACTDEF \"in\",2,1.0,2.0,0.5,1.1; FORM_SETACT \"in\"; CLIP; FORM_PUXY 1,2;\
              NRL \"tex\",\"p\",1,2; SHADOW; PART_BILINEAR; SHADOW 1,2;\
              FILLBOX \"sq_beta\",\"fill\",0,0,512,480,0; FILLBOXINFO 40,29,42,128;\
              PART_DTYPE \"clut_reload\",\"c\",5; PARTS_EFF_NUM 1; PARTS_EFFECT \"blink\",1,2; FORM_SETEND;",
        );
        let form = &doc["operations"][0];
        assert_eq!(form["slot"], 3);
        assert_eq!(
            form["settings"][2],
            json!({"set": "other_form_draw_flag", "form": "g", "draw": 1})
        );
        assert_eq!(form["settings"][3]["actions"][0]["rate"], json!([0.5, 1.1]));
        assert_eq!(form["settings"][5], json!({"set": "clip"}));
        let parts = form["parts"].as_array().unwrap();
        assert_eq!(parts[0]["xy"], json!([1, 2]));
        assert!(parts[0].get("size").is_none());
        assert_eq!(
            parts[0]["modifiers"][2],
            json!({"mod": "shadow", "on": 1, "offset": 2})
        );
        assert_eq!(
            parts[1]["modifiers"][0],
            json!({"mod": "fill", "rgba": [40, 29, 42, 128]})
        );
        assert_eq!(parts[2]["etc_info"], json!([5]));
        assert_eq!(parts[2]["modifiers"][1]["params"], json!([1, 2]));
    }

    #[test]
    fn layout_refuses_shapes_retail_never_writes() {
        let error = refuse(LAYOUT_ALIAS, b"SHADOW;");
        assert!(error.contains("no form or part"), "{error}");
        let error = refuse(
            LAYOUT_ALIAS,
            b"FORM_SET \"a\"; NRL \"t\",\"p\",0,0; FORM_PUTXY 1,2;",
        );
        assert!(error.contains("follows the form's parts"), "{error}");
        let error = refuse(LAYOUT_ALIAS, b"FORM_SET \"a\"; FORM_DTYPE \"geolist\";");
        assert!(error.contains("stale"), "{error}");
    }

    #[test]
    fn commands_group_blocks_and_keep_stale_slots() {
        let doc = convert(
            COMMAND_ALIAS,
            b"EXE_NAME \"open\"; FRM_DRAWFLG 1,\"a\",\"b\"; FRM_RGBA \"x\"; FRM_RGBA \"y\",1,2,3,4;\
              CURSOR_FADE 0,16,0; FRM_PTSONOFF \"f\",\"p\",1; SE_OK \"x\"; EXE_NAME \"close\";\
              FRM_SETPOS \"f\",1,2; MSG_SPD 0,1.5; EXE_NAME \"open\"; SCN_FADE 1;",
        );
        let blocks = doc["blocks"].as_array().unwrap();
        assert_eq!(blocks.len(), 3);
        let open = blocks[0]["commands"].as_array().unwrap();
        assert_eq!(
            open[0],
            json!({"op": "form_draw_flag", "draw": 1, "forms": ["a", "b"]})
        );
        assert_eq!(open[1], json!({"op": "form_rgba", "form": "x"}));
        assert_eq!(open[2]["rgba"], json!([1, 2, 3, 4]));
        assert_eq!(
            open[3],
            json!({"op": "cursor_fade", "fade_in": 0, "speed": 16, "steps": 0})
        );
        assert_eq!(open[4]["spelling"], "FRM_PTSONOFF");
        assert_eq!(
            blocks[1]["commands"][0],
            json!({"op": "form_set_pos", "form": "f", "xy": [1, 2]})
        );
        assert_eq!(blocks[2]["name"], "open");
        // A short FRM_RGBA reads the previous command's slots.
        let doc = convert(
            COMMAND_ALIAS,
            b"EXE_NAME \"a\"; FRM_CALCRGBA \"f\",1,2,3; FRM_RGBA \"g\",9;",
        );
        assert_eq!(doc["blocks"][0]["commands"][1]["rgba"], json!([9, 2, 3, 0]));
    }

    #[test]
    fn commands_before_the_first_name_are_refused() {
        let error = refuse(COMMAND_ALIAS, b"SE_ON \"OK\"; EXE_NAME \"a\";");
        assert!(error.contains("before the first EXE_NAME"), "{error}");
    }
}
