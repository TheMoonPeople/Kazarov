//! Dungeon tables: automap room presets, monster placement, treasure boxes, monster names and the
//! tree map of each dungeon with its floor details.
use super::*;

/// Monster pairs per `FL` row, the retail scripts' line width.
const MONSTERS_PER_ROW: usize = 8;
/// Item triples per `ITEM` row, the retail scripts' line width.
const ITEMS_PER_ROW: usize = 5;

/// Splits a flat list of values into rows of at most `width` entries; no rows for no values.
fn chunked(values: Vec<Option<Val>>, width: usize) -> Vec<Vec<Option<Val>>> {
    values.chunks(width).map(<[Option<Val>]>::to_vec).collect()
}

fn int_list(value: &Value, key: &str) -> CoreResult<Vec<Option<Val>>> {
    array(value, key)?
        .iter()
        .map(|v| int_val(v).map(Some))
        .collect()
}

fn int_rest(eff: &Eff, from: usize) -> CoreResult<Value> {
    ints_json(eff, from, eff.count().saturating_sub(from))
}

// --- dungeon_rooms: CAutoMapGen::SetupRoomInfo (automap.cpp) --------------------------------------

const ROOM_TAG: Pass = Pass {
    name: "tag",
    commands: &[
        Command {
            name: "ROOM_FIXED",
            layout: "i",
        },
        Command {
            name: "GRID_SIZE",
            layout: "f f",
        },
        Command {
            name: "ROOM_ID",
            layout: "i",
        },
        // Reads its two operands only when it has exactly two.
        Command {
            name: "ROOM_SIZE",
            layout: "*i",
        },
        Command {
            name: "ROOM_RATE",
            layout: "i",
        },
        // Reads its operands only when there are two per cell of the room's width.
        Command {
            name: "RD",
            layout: "*i",
        },
        Command {
            name: "ROOM_END",
            layout: "",
        },
    ],
};

fn build_rooms(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let mut out = Obj::new();
    if let Some(grid) = cursor.take("GRID_SIZE") {
        out = out
            .put("cell_w", float_json(grid.float(0)?))
            .put("cell_d", float_json(grid.float(1)?));
    }
    let mut rooms = Vec::new();
    while !cursor.done() {
        let id = cursor.expect("ROOM_ID")?.int(0)?;
        let size = cursor.expect("ROOM_SIZE")?;
        if size.count() != 2 {
            return Err(fail("ROOM_SIZE needs a width and a height"));
        }
        let (w, h) = (size.int(0)?, size.int(1)?);
        let rate = cursor.expect("ROOM_RATE")?.int(0)?;
        let fixed = cursor.expect("ROOM_FIXED")?.int(0)?;
        let mut table = Vec::new();
        while let Some(row) = cursor.take("RD") {
            if i64::try_from(row.count()).ok() != Some(i64::from(w) * 2) {
                return Err(fail(format!(
                    "RD row of {} values in a room {w} wide",
                    row.count()
                )));
            }
            let cells = (0..row.count() / 2)
                .map(|cell| Ok(json!([row.int(cell * 2)?, row.int(cell * 2 + 1)?])))
                .collect::<CoreResult<Vec<_>>>()?;
            table.push(Value::Array(cells));
        }
        if i64::try_from(table.len()).ok() != Some(i64::from(h)) {
            return Err(fail(format!("room {id} has {} rows, not {h}", table.len())));
        }
        cursor.expect("ROOM_END")?;
        rooms.push(json!({"id": id, "w": w, "h": h, "rate": rate, "fixed": fixed, "table": table}));
    }
    Ok(out.put("rooms", Value::Array(rooms)).done())
}

fn flatten_rooms(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    match (document.get("cell_w"), document.get("cell_d")) {
        (Some(_), Some(_)) => rows.push(Eff::new(
            "GRID_SIZE",
            vec![
                Some(float_field(document, "cell_w")?),
                Some(float_field(document, "cell_d")?),
            ],
        )),
        (None, None) => {}
        _ => return Err(fail("cell_w and cell_d go together")),
    }
    for room in array(document, "rooms")? {
        rows.push(Eff::new("ROOM_ID", vec![Some(int_field(room, "id")?)]));
        rows.push(Eff::new(
            "ROOM_SIZE",
            vec![Some(int_field(room, "w")?), Some(int_field(room, "h")?)],
        ));
        rows.push(Eff::new("ROOM_RATE", vec![Some(int_field(room, "rate")?)]));
        rows.push(Eff::new(
            "ROOM_FIXED",
            vec![Some(int_field(room, "fixed")?)],
        ));
        for row in array(room, "table")? {
            let mut vals = Vec::new();
            for cell in row
                .as_array()
                .ok_or_else(|| fail("room table row is not an array"))?
            {
                let pair = cell
                    .as_array()
                    .filter(|pair| pair.len() == 2)
                    .ok_or_else(|| fail("room cell needs a part and an attribute"))?;
                vals.push(Some(int_val(&pair[0])?));
                vals.push(Some(int_val(&pair[1])?));
            }
            rows.push(Eff::new("RD", vals));
        }
        rows.push(Eff::new("ROOM_END", vec![]));
    }
    Ok(vec![rows])
}

const DUNGEON_ROOMS: Family = Family {
    kind: "dungeon_rooms",
    passes: &[&ROOM_TAG],
    ignored: &["VER"],
    build: build_rooms,
    flatten: flatten_rooms,
    ordered: true,
};

// --- monster_floors: CreatMonsterFloorInfo (dng_event.cpp) ----------------------------------------

const MONSTER_TAG: Pass = Pass {
    name: "tag2",
    commands: &[
        // Its second operand, the floor's monster count, is read and discarded.
        Command {
            name: "FLS",
            layout: "i",
        },
        Command {
            name: "FL",
            layout: "*i",
        },
        Command {
            name: "FLE",
            layout: "",
        },
    ],
};

fn build_monsters(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let mut floors = Vec::new();
    while !cursor.done() {
        let floor = cursor.expect("FLS")?.int(0)?;
        let mut monsters = Vec::new();
        while let Some(row) = cursor.take("FL") {
            if !row.count().is_multiple_of(2) {
                return Err(fail("FL row with an unpaired value"));
            }
            for pair in 0..row.count() / 2 {
                monsters.push(
                    json!({"monster_id": row.int(pair * 2)?, "param": row.int(pair * 2 + 1)?}),
                );
            }
        }
        cursor.expect("FLE")?;
        floors.push(json!({"floor": floor, "monsters": monsters}));
    }
    Ok(json!({ "floors": floors }))
}

fn flatten_monsters(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for floor in array(document, "floors")? {
        rows.push(Eff::new("FLS", vec![Some(int_field(floor, "floor")?)]));
        let mut vals = Vec::new();
        for monster in array(floor, "monsters")? {
            vals.push(Some(int_field(monster, "monster_id")?));
            vals.push(Some(int_field(monster, "param")?));
        }
        for row in chunked(vals, MONSTERS_PER_ROW * 2) {
            rows.push(Eff::new("FL", row));
        }
        rows.push(Eff::new("FLE", vec![]));
    }
    Ok(vec![rows])
}

const MONSTER_FLOORS: Family = Family {
    kind: "monster_floors",
    passes: &[&MONSTER_TAG],
    ignored: &[],
    build: build_monsters,
    flatten: flatten_monsters,
    ordered: true,
};

// --- treasure_boxes: CreatTresuarBoxInfo (dng_event.cpp) ------------------------------------------

const TBOX_TAG: Pass = Pass {
    name: "tag__5",
    commands: &[
        // Reads one operand and discards it; it only begins the group list.
        Command {
            name: "GROUP_START",
            layout: "",
        },
        Command {
            name: "GROUP",
            layout: "i i",
        },
        Command {
            name: "ITEM",
            layout: "*i",
        },
        Command {
            name: "FLOOR_START",
            layout: "i",
        },
        Command {
            name: "FLOOR",
            layout: "i i *i",
        },
    ],
};

fn build_tboxes(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    cursor.expect("GROUP_START")?;
    let mut groups = Vec::new();
    while let Some(group) = cursor.take("GROUP") {
        let mut items = Vec::new();
        while let Some(row) = cursor.take("ITEM") {
            if !row.count().is_multiple_of(3) {
                return Err(fail("ITEM row with an incomplete item"));
            }
            for item in 0..row.count() / 3 {
                items.push(json!({
                    "item_no": row.int(item * 3)?,
                    "rank": row.int(item * 3 + 1)?,
                    "num": row.int(item * 3 + 2)?,
                }));
            }
        }
        groups.push(json!({"group_id": group.int(0)?, "item_num": group.int(1)?, "items": items}));
    }
    let floor_start = cursor.expect("FLOOR_START")?.int(0)?;
    let mut floors = Vec::new();
    while let Some(row) = cursor.take("FLOOR") {
        let ids = int_rest(row, 2)?;
        if i64::try_from(row.count() - 2).ok() != Some(i64::from(row.int(1)?)) {
            return Err(fail("FLOOR group count differs from its groups"));
        }
        floors.push(json!({"floor": row.int(0)?, "group_id": ids}));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"groups": groups, "floor_start": floor_start, "floors": floors}))
}

fn flatten_tboxes(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new("GROUP_START", vec![])];
    for group in array(document, "groups")? {
        rows.push(Eff::new(
            "GROUP",
            vec![
                Some(int_field(group, "group_id")?),
                Some(int_field(group, "item_num")?),
            ],
        ));
        let mut vals = Vec::new();
        for item in array(group, "items")? {
            vals.push(Some(int_field(item, "item_no")?));
            vals.push(Some(int_field(item, "rank")?));
            vals.push(Some(int_field(item, "num")?));
        }
        for row in chunked(vals, ITEMS_PER_ROW * 3) {
            rows.push(Eff::new("ITEM", row));
        }
    }
    rows.push(Eff::new(
        "FLOOR_START",
        vec![Some(int_field(document, "floor_start")?)],
    ));
    for floor in array(document, "floors")? {
        let ids = int_list(floor, "group_id")?;
        let count = i32::try_from(ids.len()).map_err(|_| fail("too many groups"))?;
        let mut vals = vec![Some(int_field(floor, "floor")?), Some(Val::I(count))];
        vals.extend(ids);
        rows.push(Eff::new("FLOOR", vals));
    }
    Ok(vec![rows])
}

const TREASURE_BOXES: Family = Family {
    kind: "treasure_boxes",
    passes: &[&TBOX_TAG],
    ignored: &[],
    build: build_tboxes,
    flatten: flatten_tboxes,
    ordered: true,
};

// --- monster_names: LoadMonsterLanguage (monster.cpp) ---------------------------------------------

const NAME_TAG: Pass = Pass {
    name: "mos_data_anlyze_tag",
    commands: &[Command {
        name: "NAME",
        layout: "i s",
    }],
};

fn build_names(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let names = passes[0]
        .iter()
        .map(|row| Ok(json!({"id": row.int(0)?, "name": text_json(&row.text(1)?)})))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(json!({ "names": names }))
}

fn flatten_names(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "names")?
        .iter()
        .map(|name| row_of(name, "NAME", &[("id", 'i'), ("name", 's')]))
        .collect::<CoreResult<Vec<_>>>()?;
    if rows.iter().any(|row| row.vals.iter().any(Option::is_none)) {
        return Err(fail("monster name needs an id and a name"));
    }
    Ok(vec![rows])
}

const MONSTER_NAMES: Family = Family {
    kind: "monster_names",
    passes: &[&NAME_TAG],
    ignored: &[],
    build: build_names,
    flatten: flatten_names,
    ordered: true,
};

// --- dungeon_map / dungeon_floors: CDngFloorManager::AnalyzeFile (dngfloor.cpp) -------------------

const TREE_MAP_TAG: Pass = Pass {
    name: "tree_map_tag",
    commands: &[
        Command {
            name: "TREE_INFO",
            layout: "i i i",
        },
        Command {
            name: "GI",
            layout: "i i i i i",
        },
        Command {
            name: "RT",
            layout: "i i i",
        },
        // The third operand is read and discarded.
        Command {
            name: "RI",
            layout: "i i i s?3",
        },
        Command {
            name: "RI_LINK",
            layout: "*i",
        },
        // Each operand is an option name; with more than one, the room offset comes from the slots
        // after them, which the corpus never does.
        Command {
            name: "RI_OP",
            layout: "*s",
        },
        Command {
            name: "RI_TEX",
            layout: "i",
        },
        Command {
            name: "RI_KEYROOM",
            layout: "i i i i",
        },
        Command {
            name: "RF_INFO",
            layout: "i i i i i i i i i i i i i",
        },
        Command {
            name: "RF_INFO2",
            layout: "i i i",
        },
        Command {
            name: "RI_TITLE",
            layout: "i s",
        },
    ],
};

fn build_room(cursor: &mut Cursor, ri: &Eff) -> CoreResult<Value> {
    let mut room = Obj::new()
        .put("floor_id", json!(ri.int(0)?))
        .put("order", json!(ri.int(1)?));
    let unused = ri.int(2)?;
    if unused != 0 {
        room = room.put("unused", json!(unused));
    }
    if ri.has(3) {
        room = room.put("name", text_json(&ri.text(3)?));
    }
    if let Some(link) = cursor.take("RI_LINK") {
        room = room.put("link", int_rest(link, 0)?);
    }
    if let Some(tex) = cursor.take("RI_TEX") {
        room = room.put("tex_no", json!(tex.int(0)?));
    }
    if let Some(key) = cursor.take("RI_KEYROOM") {
        room = room.put("key_room", ints_json(key, 0, 4)?);
    }
    let mut options = Vec::new();
    while let Some(option) = cursor.take("RI_OP") {
        if option.count() != 1 {
            return Err(fail("RI_OP with other than one option"));
        }
        options.push(text_json(&option.text(0)?));
    }
    if !options.is_empty() {
        room = room.put("options", Value::Array(options));
    }
    Ok(room.done())
}

fn build_map(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let tree = cursor.expect("TREE_INFO")?;
    let mut cells = Vec::new();
    while let Some(gi) = cursor.take("GI") {
        let mut cell = Obj::new()
            .put("type", json!(gi.int(0)?))
            .put("x", json!(gi.int(1)?))
            .put("y", json!(gi.int(2)?))
            .put("unk_6", json!(gi.int(3)?))
            .put("unk_8", json!(gi.int(4)?));
        if let Some(rt) = cursor.take("RT") {
            cell = cell.put(
                "root",
                json!({"type": rt.int(0)?, "shape": rt.int(1)?, "show_mark": rt.int(2)?}),
            );
        } else if let Some(ri) = cursor.take("RI") {
            cell = cell.put("room", build_room(&mut cursor, ri)?);
        }
        cells.push(cell.done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    if i64::try_from(cells.len()).ok() != Some(i64::from(tree.int(2)?)) {
        return Err(fail("TREE_INFO cell count differs from its cells"));
    }
    Ok(json!({"glid_w": tree.int(0)?, "glid_h": tree.int(1)?, "cells": cells}))
}

fn flatten_room(room: &Value, rows: &mut Vec<Eff>) -> CoreResult<()> {
    let unused = match room.get("unused") {
        Some(value) => int_val(value)?,
        None => Val::I(0),
    };
    rows.push(Eff::new(
        "RI",
        vec![
            Some(int_field(room, "floor_id")?),
            Some(int_field(room, "order")?),
            Some(unused),
            opt_field(room, "name", text_val)?,
        ],
    ));
    if room.get("link").is_some() {
        rows.push(Eff::new("RI_LINK", int_list(room, "link")?));
    }
    if let Some(tex) = opt_field(room, "tex_no", int_val)? {
        rows.push(Eff::new("RI_TEX", vec![Some(tex)]));
    }
    if room.get("key_room").is_some() {
        rows.push(Eff::new("RI_KEYROOM", int_slots(room, "key_room", 4)?));
    }
    if room.get("options").is_some() {
        for option in array(room, "options")? {
            rows.push(Eff::new("RI_OP", vec![Some(text_val(option)?)]));
        }
    }
    Ok(())
}

fn flatten_map(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let cells = array(document, "cells")?;
    let count = i32::try_from(cells.len()).map_err(|_| fail("too many cells"))?;
    let mut rows = vec![Eff::new(
        "TREE_INFO",
        vec![
            Some(int_field(document, "glid_w")?),
            Some(int_field(document, "glid_h")?),
            Some(Val::I(count)),
        ],
    )];
    for cell in cells {
        rows.push(row_of(
            cell,
            "GI",
            &[
                ("type", 'i'),
                ("x", 'i'),
                ("y", 'i'),
                ("unk_6", 'i'),
                ("unk_8", 'i'),
            ],
        )?);
        if rows
            .last()
            .is_some_and(|row| row.vals.iter().any(Option::is_none))
        {
            return Err(fail("cell needs type, x, y, unk_6 and unk_8"));
        }
        match (cell.get("root"), cell.get("room")) {
            (Some(root), None) => rows.push(Eff::new(
                "RT",
                vec![
                    Some(int_field(root, "type")?),
                    Some(int_field(root, "shape")?),
                    Some(int_field(root, "show_mark")?),
                ],
            )),
            (None, Some(room)) => flatten_room(room, &mut rows)?,
            (None, None) => {}
            (Some(_), Some(_)) => return Err(fail("a cell is a root or a room, not both")),
        }
    }
    Ok(vec![rows])
}

const DUNGEON_MAP: Family = Family {
    kind: "dungeon_map",
    passes: &[&TREE_MAP_TAG],
    ignored: &[],
    build: build_map,
    flatten: flatten_map,
    ordered: true,
};

const FLOOR_INFO_KEYS: [&str; 6] = [
    "geostone",
    "fast_destroy_time",
    "fishing",
    "fishing_record",
    "seal",
    "spheda",
];

fn build_floors(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut floors: Vec<(i32, Obj)> = Vec::new();
    for row in &passes[0] {
        let floor = row.int(0)?;
        let at = match floors.iter().position(|(id, _)| *id == floor) {
            Some(at) => at,
            None => {
                floors.push((floor, Obj::new().put("floor", json!(floor))));
                floors.len() - 1
            }
        };
        let key = match row.name.as_str() {
            "RF_INFO" => "info",
            "RF_INFO2" => "practice",
            other => return Err(fail(format!("{other} in a floor table"))),
        };
        if floors[at].1.0.contains_key(key) {
            return Err(fail(format!("floor {floor} has two {} rows", row.name)));
        }
        let value = if key == "info" {
            let mut info = Obj::new();
            for (slot, name) in FLOOR_INFO_KEYS.iter().enumerate() {
                info = info.put(name, json!(row.int(slot + 1)?));
            }
            let items = (0..3)
                .map(|i| row.int(7 + i * 2))
                .collect::<CoreResult<Vec<_>>>()?;
            let nums = (0..3)
                .map(|i| row.int(8 + i * 2))
                .collect::<CoreResult<Vec<_>>>()?;
            info.put("spheda_prize_item", json!(items))
                .put("spheda_prize_num", json!(nums))
                .done()
        } else {
            json!({"type": row.int(1)?, "param": row.int(2)?})
        };
        let entry = std::mem::replace(&mut floors[at].1, Obj::new());
        floors[at].1 = entry.put(key, value);
    }
    let floors = floors
        .into_iter()
        .map(|(_, obj)| obj.done())
        .collect::<Vec<_>>();
    Ok(json!({ "floors": floors }))
}

fn flatten_floors(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for floor in array(document, "floors")? {
        let id = int_field(floor, "floor")?;
        if let Some(info) = floor.get("info") {
            let mut vals = vec![Some(id.clone())];
            for name in FLOOR_INFO_KEYS {
                vals.push(Some(int_field(info, name)?));
            }
            let items = int_slots(info, "spheda_prize_item", 3)?;
            let nums = int_slots(info, "spheda_prize_num", 3)?;
            for (item, num) in items.into_iter().zip(nums) {
                vals.push(item);
                vals.push(num);
            }
            rows.push(Eff::new("RF_INFO", vals));
        }
        if let Some(practice) = floor.get("practice") {
            rows.push(Eff::new(
                "RF_INFO2",
                vec![
                    Some(id),
                    Some(int_field(practice, "type")?),
                    Some(int_field(practice, "param")?),
                ],
            ));
        }
    }
    Ok(vec![rows])
}

const DUNGEON_FLOORS: Family = Family {
    kind: "dungeon_floors",
    passes: &[&TREE_MAP_TAG],
    ignored: &[],
    build: build_floors,
    flatten: flatten_floors,
    ordered: false,
};

pub(super) const FAMILIES: &[&Family] = &[
    &DUNGEON_ROOMS,
    &MONSTER_FLOORS,
    &TREASURE_BOXES,
    &MONSTER_NAMES,
    &DUNGEON_MAP,
    &DUNGEON_FLOORS,
];

/// Whether `text` is `prefix` followed by exactly `digits` ASCII digits and then `suffix`.
fn numbered(text: &str, prefix: &str, digits: usize, suffix: &str) -> bool {
    text.strip_prefix(prefix)
        .and_then(|rest| rest.strip_suffix(suffix))
        .is_some_and(|number| number.len() == digits && number.bytes().all(|b| b.is_ascii_digit()))
}

/// Whether a leaf is a dungeon room preset name: `d??f??.cfg`, optionally prefixed with `_`.
fn room_preset(leaf: &str) -> bool {
    let name = leaf.strip_prefix('_').unwrap_or(leaf);
    name.len() == 10
        && name.ends_with(".cfg")
        && name.as_bytes()[0] == b'd'
        && name.as_bytes()[3] == b'f'
        && [1, 2, 4, 5]
            .iter()
            .all(|&i| name.as_bytes()[i].is_ascii_digit())
}

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    if path.strip_prefix("dungeon/cfg_file/") == Some(leaf) {
        if room_preset(leaf) || numbered(leaf, "mstage", 1, ".cfg") {
            return Some("dungeon_rooms");
        }
        if numbered(leaf, "mos_place", 1, ".cfg") {
            return Some("monster_floors");
        }
        if numbered(leaf, "tbox_d", 2, ".cfg") {
            return Some("treasure_boxes");
        }
        if numbered(leaf, "mosdata", 1, ".cfg") {
            return Some("monster_names");
        }
        return None;
    }
    if path.strip_prefix("menu/dngmap/") == Some(leaf) {
        if numbered(leaf, "dmap", 1, ".cfg") {
            return Some("dungeon_map");
        }
        if numbered(leaf, "dflr", 1, ".cfg") {
            return Some("dungeon_floors");
        }
        return None;
    }
    // menu/<language>/dmap<n>.pac/dmap<n>.cfg: the pack's copy of the dungeon's tree map.
    let mut parts = path.split('/');
    if let (Some("menu"), Some(language), Some(pack), Some(member), None) = (
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
        parts.next(),
    ) && language.len() == 1
        && language.bytes().all(|b| b.is_ascii_digit())
        && numbered(pack, "dmap", 1, ".pac")
        && member.strip_suffix(".cfg") == pack.strip_suffix(".pac")
    {
        return Some("dungeon_map");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> CoreResult<Value> {
        let document = super::super::super::configs::parse(script).unwrap();
        super::super::build(alias, &document).map(|(doc, _)| doc)
    }

    #[test]
    fn families_claim_their_paths() {
        let cases = [
            ("dungeon/cfg_file/d01f01.cfg", Some("dungeon_rooms")),
            ("dungeon/cfg_file/_d03f03.cfg", Some("dungeon_rooms")),
            ("dungeon/cfg_file/mstage8.cfg", Some("dungeon_rooms")),
            ("dungeon/cfg_file/mos_place6.cfg", Some("monster_floors")),
            ("dungeon/cfg_file/tbox_d08.cfg", Some("treasure_boxes")),
            ("dungeon/cfg_file/mosdata3.cfg", Some("monster_names")),
            ("menu/dngmap/dmap4.cfg", Some("dungeon_map")),
            ("menu/dngmap/dflr4.cfg", Some("dungeon_floors")),
            ("menu/3/dmap4.pac/dmap4.cfg", Some("dungeon_map")),
            ("menu/3/dmap4.pac/dtmap_com.cfg", None),
            ("menu/3/dmap4.pac/dmap3.cfg", None),
            ("dungeon/cfg_file/other.cfg", None),
        ];
        for (path, expected) in cases {
            let leaf = path.rsplit('/').next().unwrap();
            assert_eq!(kind(path, leaf), expected, "{path}");
        }
    }

    #[test]
    fn room_presets_nest_rows_into_rooms() {
        let doc = convert(
            "dungeon/cfg_file/d01f01.cfg",
            b"VER 1.01; GRID_SIZE 320.0,160; ROOM_ID 3; ROOM_SIZE 2,2; ROOM_RATE 50; ROOM_FIXED 1;\
              RD 1,0,2,0; RD -1,0,3,4; ROOM_END; ROOM_ID 4; ROOM_SIZE 1,1; ROOM_RATE 100; ROOM_FIXED 0;\
              RD 9,9; ROOM_END;",
        )
        .unwrap();
        assert_eq!(doc["kind"], "dungeon_rooms");
        assert_eq!(doc["cell_w"], json!(320.0));
        assert_eq!(doc["cell_d"], json!(160.0));
        assert_eq!(
            doc["rooms"][0],
            json!({"id":3,"w":2,"h":2,"rate":50,"fixed":1,"table":[[[1,0],[2,0]],[[-1,0],[3,4]]]})
        );
        assert_eq!(doc["rooms"][1]["table"], json!([[[9, 9]]]));
        // GRID_SIZE is optional.
        let doc = convert(
            "dungeon/cfg_file/d02f01.cfg",
            b"ROOM_ID 0; ROOM_SIZE 1,1; ROOM_RATE 1; ROOM_FIXED 0; RD 1,2; ROOM_END;",
        )
        .unwrap();
        assert!(doc.get("cell_w").is_none());
    }

    #[test]
    fn room_rows_must_match_the_room_size() {
        for script in [
            &b"ROOM_ID 0; ROOM_SIZE 2,1; ROOM_RATE 1; ROOM_FIXED 0; RD 1,2; ROOM_END;"[..],
            b"ROOM_ID 0; ROOM_SIZE 1,2; ROOM_RATE 1; ROOM_FIXED 0; RD 1,2; ROOM_END;",
            b"ROOM_ID 0; ROOM_SIZE 1; ROOM_RATE 1; ROOM_FIXED 0; ROOM_END;",
            b"ROOM_ID 0; ROOM_SIZE 1,1; ROOM_RATE 1; ROOM_FIXED 0; RD 1,2;",
        ] {
            assert!(convert("dungeon/cfg_file/d01f01.cfg", script).is_err());
        }
    }

    #[test]
    fn monster_floors_merge_rows_and_keep_line_widths() {
        let long: String = (0..9)
            .map(|i| format!("{i},{}", i * 10))
            .collect::<Vec<_>>()
            .join(",");
        let script = format!(
            "FLS 1,18; FL 0,0,0,1,0,2,0,3,0,4,0,5,8,800,8,805; FL 8,804; FLE; FLS 2,9; FL {long}; FLE;"
        );
        // A row of nine pairs flattens back as eight and one, so it cannot be represented.
        assert!(convert("dungeon/cfg_file/mos_place0.cfg", script.as_bytes()).is_err());
        let doc = convert(
            "dungeon/cfg_file/mos_place0.cfg",
            b"FLS 1,18; FL 0,0,0,1,0,2,0,3,0,4,0,5,8,800,8,805; FL 8,804; FLE; FLS 2; FLE;",
        )
        .unwrap();
        assert_eq!(doc["floors"][0]["floor"], 1);
        assert_eq!(doc["floors"][0]["monsters"].as_array().unwrap().len(), 9);
        assert_eq!(
            doc["floors"][0]["monsters"][8],
            json!({"monster_id":8,"param":804})
        );
        assert_eq!(doc["floors"][1]["monsters"], json!([]));
        assert!(convert("dungeon/cfg_file/mos_place0.cfg", b"FLS 1; FL 1,2,3; FLE;").is_err());
        assert!(convert("dungeon/cfg_file/mos_place0.cfg", b"FL 1,2; FLE;").is_err());
    }

    #[test]
    fn treasure_boxes_nest_items_and_floors() {
        let doc = convert(
            "dungeon/cfg_file/tbox_d01.cfg",
            b"GROUP_START 5,29; GROUP 12,3; ITEM 1,15,1,22,5,1,23,5,1; GROUP 13,6;\
              ITEM 268,60,3,275,70,2,280,90,2,294,60,2,297,20,1; ITEM 298,60,2;\
              FLOOR_START 5,19; FLOOR 1,3,12,13,44; FLOOR 2,0;",
        )
        .unwrap();
        assert_eq!(doc["kind"], "treasure_boxes");
        assert_eq!(doc["groups"][0]["group_id"], 12);
        assert_eq!(doc["groups"][0]["item_num"], 3);
        assert_eq!(
            doc["groups"][0]["items"][1],
            json!({"item_no":22,"rank":5,"num":1})
        );
        assert_eq!(doc["groups"][1]["items"].as_array().unwrap().len(), 6);
        assert_eq!(doc["floor_start"], 5);
        assert_eq!(doc["floors"][0], json!({"floor":1,"group_id":[12,13,44]}));
        assert_eq!(doc["floors"][1], json!({"floor":2,"group_id":[]}));
        let empty = convert(
            "dungeon/cfg_file/tbox_d08.cfg",
            b"GROUP_START 0,0; FLOOR_START 0,103;",
        )
        .unwrap();
        assert_eq!(empty["groups"], json!([]));
        assert!(
            convert(
                "dungeon/cfg_file/tbox_d01.cfg",
                b"GROUP_START; FLOOR_START 1; FLOOR 1,2,3;"
            )
            .is_err()
        );
        assert!(
            convert(
                "dungeon/cfg_file/tbox_d01.cfg",
                b"GROUP 1,1; FLOOR_START 1;"
            )
            .is_err()
        );
    }

    #[test]
    fn monster_names_keep_text() {
        let doc = convert(
            "dungeon/cfg_file/mosdata0.cfg",
            b"NAME 0,\"Rat\"; NAME 3,x;",
        )
        .unwrap();
        assert_eq!(doc["names"][0], json!({"id":0,"name":"Rat"}));
        assert_eq!(doc["names"][1], json!({"id":3,"name":null}));
    }

    #[test]
    fn dungeon_maps_nest_cells() {
        let doc = convert(
            "menu/dngmap/dmap0.cfg",
            "TREE_INFO 10,10,3; GI 1,0,0,0,0; RI 0,0,0,\"Entry\"; RI_LINK -1,-1,-1,1; RI_TEX -2;\
             RI_KEYROOM -1,1,-1,1; RI_OP \"start\"; GI 0,1,0,1,0; RT 0,1,0; GI 1,2,0,2,0; RI 1,1,2;"
                .as_bytes(),
        )
        .unwrap();
        assert_eq!(doc["kind"], "dungeon_map");
        assert_eq!(
            (doc["glid_w"].clone(), doc["glid_h"].clone()),
            (json!(10), json!(10))
        );
        assert_eq!(
            doc["cells"][0]["room"],
            json!({"floor_id":0,"order":0,"name":"Entry","link":[-1,-1,-1,1],"tex_no":-2,
                   "key_room":[-1,1,-1,1],"options":["start"]})
        );
        assert_eq!(
            doc["cells"][1]["root"],
            json!({"type":0,"shape":1,"show_mark":0})
        );
        assert_eq!(
            doc["cells"][2]["room"],
            json!({"floor_id":1,"order":1,"unused":2})
        );
        assert_eq!(doc["cells"][2]["x"], 2);
        // The same document comes from the menu pack's copy.
        let pack = convert(
            "menu/0/dmap0.pac/dmap0.cfg",
            b"TREE_INFO 1,1,1; GI 0,0,0,0,0;",
        )
        .unwrap();
        assert_eq!(
            pack["cells"][0],
            json!({"type":0,"x":0,"y":0,"unk_6":0,"unk_8":0})
        );
        for bad in [
            &b"TREE_INFO 1,1,2; GI 0,0,0,0,0;"[..],
            b"TREE_INFO 1,1,1; GI 0,0,0,0,0; RI 0,0,0; RT 0,0,0;",
            b"TREE_INFO 1,1,1; GI 1,0,0,0,0; RI 0,0,0; RI_OP \"sub\",1,2;",
            b"TREE_INFO 1,1,1; GI 1,0,0,0,0; RF_INFO2 1,2,3;",
        ] {
            assert!(convert("menu/dngmap/dmap0.cfg", bad).is_err());
        }
    }

    #[test]
    fn dungeon_floors_merge_both_rows() {
        let doc = convert(
            "menu/dngmap/dflr0.cfg",
            b"RF_INFO 1,0,9300,0,0,0,1,297,2,273,2,272,2; RF_INFO2 1,5,0; RF_INFO2 2,1,7;",
        )
        .unwrap();
        assert_eq!(doc["kind"], "dungeon_floors");
        assert_eq!(
            doc["floors"][0],
            json!({"floor":1,"info":{"geostone":0,"fast_destroy_time":9300,"fishing":0,"fishing_record":0,
                   "seal":0,"spheda":1,"spheda_prize_item":[297,273,272],"spheda_prize_num":[2,2,2]},
                   "practice":{"type":5,"param":0}})
        );
        assert_eq!(
            doc["floors"][1],
            json!({"floor":2,"practice":{"type":1,"param":7}})
        );
        assert!(convert("menu/dngmap/dflr0.cfg", b"RF_INFO2 1,5,0; RF_INFO2 1,5,0;").is_err());
        assert!(convert("menu/dngmap/dflr0.cfg", b"GI 1,0,0,0,0;").is_err());
    }
}
