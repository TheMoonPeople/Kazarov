//! Item tables read by `LoadGameDataAnalyze` and `CGameData::LoadItemSystemMes` (gamedata.cpp):
//! common item records, weapons, usable items, attachments, ridepod parts, fish, guards and item
//! names. Retail runs every table through one tag table; each published table holds one kind.
use super::*;

// --- Shared shapes -----------------------------------------------------------------------------

/// Reads an optional leading count command (the table's `*INIT`/`*NUM`), which retail stores as the
/// table's count independently of the rows that follow.
fn leading_count(cursor: &mut Cursor, name: &str) -> CoreResult<Option<i32>> {
    cursor.take(name).map(|row| row.int(0)).transpose()
}

/// Reads the count command that must open a table.
fn required_count(cursor: &mut Cursor, name: &str) -> CoreResult<i32> {
    cursor.expect(name)?.int(0)
}

/// Converts one row's read slots to the named JSON fields of a record.
fn record(row: &Eff, keys: &[&str]) -> Value {
    row_json(row, keys)
}

/// Gives back the count command of a document's `count` field.
fn count_row(document: &Value, name: &str) -> CoreResult<Eff> {
    Ok(Eff::new(name, vec![Some(int_field(document, "count")?)]))
}

/// Gives back an optional count command.
fn optional_count_row(document: &Value, name: &str) -> CoreResult<Option<Eff>> {
    document
        .get("count")
        .map(|count| Ok(Eff::new(name, vec![Some(int_val(count)?)])))
        .transpose()
}

/// Reads a fixed-length integer array field into slots.
fn ints_of(value: &Value, key: &str, count: usize) -> CoreResult<Vec<Option<Val>>> {
    int_slots(value, key, count)
}

// --- item_common: COMINIT, COM -----------------------------------------------------------------

const COMMON: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "COMINIT",
            layout: "i",
        },
        Command {
            name: "COM",
            layout: "i i i i i i i i i s i",
        },
    ],
};

const COMMON_KEYS: &[(&str, char)] = &[
    ("item_no", 'i'),
    ("type", 'i'),
    ("list_no", 'i'),
    ("active_set", 'i'),
    ("stack_num", 'i'),
    ("max_num", 'i'),
    ("icon_texture_no", 'i'),
    ("icon_no", 'i'),
    ("message_no", 'i'),
    ("file_name", 's'),
    ("attribute", 'i'),
];

fn keys_of(keys: &[(&'static str, char)]) -> Vec<&'static str> {
    keys.iter().map(|(key, _)| *key).collect()
}

fn build_common(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = leading_count(&mut cursor, "COMINIT")?;
    let keys = keys_of(COMMON_KEYS);
    let mut records = Vec::new();
    while let Some(row) = cursor.take("COM") {
        records.push(record(row, &keys));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(Obj::new()
        .opt("count", count.map(|c| json!(c)))
        .put("records", Value::Array(records))
        .done())
}

fn flatten_common(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows: Vec<Eff> = optional_count_row(document, "COMINIT")?
        .into_iter()
        .collect();
    for entry in array(document, "records")? {
        rows.push(row_of(entry, "COM", COMMON_KEYS)?);
    }
    Ok(vec![rows])
}

const ITEM_COMMON: Family = Family {
    kind: "item_common",
    passes: &[&COMMON],
    ignored: &[],
    build: build_common,
    flatten: flatten_common,
    ordered: true,
};

// --- weapons: WEPNUM, then WEP WEP_ST WEP_ST_L WEP_ST2 WEP_ST2_L WEP_SPE WEP_BUILD per weapon -------

const WEAPON: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "WEPNUM",
            layout: "i",
        },
        Command {
            name: "WEP",
            layout: "i i",
        },
        Command {
            name: "WEP_ST",
            layout: "i i",
        },
        Command {
            name: "WEP_ST_L",
            layout: "i i",
        },
        Command {
            name: "WEP_ST2",
            layout: "i i i i i i i i",
        },
        Command {
            name: "WEP_ST2_L",
            layout: "i i i i i i i i",
        },
        Command {
            name: "WEP_SPE",
            layout: "f i i i i i? i?",
        },
        Command {
            name: "WEP_BUILD",
            layout: "i i i i?3 i?3 i?3",
        },
    ],
};

const WEAPON_SPECIAL_KEYS: &[(&str, char)] = &[
    ("initial_fusion_point", 'f'),
    ("pallet_color", 'i'),
    ("unk_47", 'i'),
    ("fusion_point", 'i'),
    ("special", 'i'),
    ("attack_type", 'i'),
    ("model_no", 'i'),
];

fn build_weapons(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "WEPNUM")?;
    let mut weapons = Vec::new();
    while !cursor.done() {
        let wep = cursor.expect("WEP")?;
        let status = cursor.expect("WEP_ST")?;
        let status_max = cursor.expect("WEP_ST_L")?;
        let attribute = cursor.expect("WEP_ST2")?;
        let attribute_max = cursor.expect("WEP_ST2_L")?;
        let special = cursor.expect("WEP_SPE")?;
        let build = cursor.expect("WEP_BUILD")?;
        let monster = if build.has(3) {
            Some(ints_json(build, 3, 3)?)
        } else {
            None
        };
        weapons.push(
            Obj::new()
                .put("durability", json!(wep.int(0)?))
                .put("levelup_exp", json!(wep.int(1)?))
                .put("status", ints_json(status, 0, 2)?)
                .put("status_max", ints_json(status_max, 0, 2)?)
                .put("attribute", ints_json(attribute, 0, 8)?)
                .put("attribute_max", ints_json(attribute_max, 0, 8)?)
                .put("special", record(special, &keys_of(WEAPON_SPECIAL_KEYS)))
                .put("buildup_weapon", ints_json(build, 0, 3)?)
                .opt("buildup_monster", monster)
                .done(),
        );
    }
    Ok(json!({"count": count, "weapons": weapons}))
}

fn flatten_weapons(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "WEPNUM")?];
    for weapon in array(document, "weapons")? {
        rows.push(Eff::new(
            "WEP",
            vec![
                Some(int_field(weapon, "durability")?),
                Some(int_field(weapon, "levelup_exp")?),
            ],
        ));
        rows.push(Eff::new("WEP_ST", ints_of(weapon, "status", 2)?));
        rows.push(Eff::new("WEP_ST_L", ints_of(weapon, "status_max", 2)?));
        rows.push(Eff::new("WEP_ST2", ints_of(weapon, "attribute", 8)?));
        rows.push(Eff::new("WEP_ST2_L", ints_of(weapon, "attribute_max", 8)?));
        rows.push(row_of(
            field(weapon, "special")?,
            "WEP_SPE",
            WEAPON_SPECIAL_KEYS,
        )?);
        let mut build = ints_of(weapon, "buildup_weapon", 3)?;
        match weapon.get("buildup_monster") {
            Some(_) => build.extend(ints_of(weapon, "buildup_monster", 3)?),
            None => build.extend([None, None, None]),
        }
        rows.push(Eff::new("WEP_BUILD", build));
    }
    Ok(vec![rows])
}

const WEAPONS: Family = Family {
    kind: "weapons",
    passes: &[&WEAPON],
    ignored: &[],
    build: build_weapons,
    flatten: flatten_weapons,
    ordered: true,
};

// --- items: ITEMINIT, ITEM ---------------------------------------------------------------------

const ITEM: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "ITEMINIT",
            layout: "i",
        },
        Command {
            name: "ITEM",
            layout: "i i i i i i i",
        },
    ],
};

fn build_items(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "ITEMINIT")?;
    let mut items = Vec::new();
    while let Some(row) = cursor.take("ITEM") {
        items.push(json!({
            "item_no": row.int(0)?,
            "use_flags": row.int(1)?,
            "action_flags": row.int(2)?,
            "target_flags": row.int(3)?,
            "value": ints_json(row, 4, 3)?,
        }));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "items": items}))
}

fn flatten_items(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "ITEMINIT")?];
    for item in array(document, "items")? {
        let mut vals = vec![
            Some(int_field(item, "item_no")?),
            Some(int_field(item, "use_flags")?),
            Some(int_field(item, "action_flags")?),
            Some(int_field(item, "target_flags")?),
        ];
        vals.extend(ints_of(item, "value", 3)?);
        rows.push(Eff::new("ITEM", vals));
    }
    Ok(vec![rows])
}

const ITEMS: Family = Family {
    kind: "items",
    passes: &[&ITEM],
    ignored: &[],
    build: build_items,
    flatten: flatten_items,
    ordered: true,
};

// --- attachments: AT_INIT, then AT_ST AT_ST2 AT_ST_SP per attachment ----------------------------

const ATTACH: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "AT_INIT",
            layout: "i",
        },
        Command {
            name: "AT_ST",
            layout: "i i",
        },
        Command {
            name: "AT_ST2",
            layout: "i i i i i i i i",
        },
        Command {
            name: "AT_ST_SP",
            layout: "i",
        },
    ],
};

fn build_attachments(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "AT_INIT")?;
    let mut attachments = Vec::new();
    while !cursor.done() {
        let status = cursor.expect("AT_ST")?;
        let attribute = cursor.expect("AT_ST2")?;
        let special = cursor.expect("AT_ST_SP")?;
        attachments.push(json!({
            "status": ints_json(status, 0, 2)?,
            "attribute": ints_json(attribute, 0, 8)?,
            "special": special.int(0)?,
        }));
    }
    Ok(json!({"count": count, "attachments": attachments}))
}

fn flatten_attachments(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "AT_INIT")?];
    for attachment in array(document, "attachments")? {
        rows.push(Eff::new("AT_ST", ints_of(attachment, "status", 2)?));
        rows.push(Eff::new("AT_ST2", ints_of(attachment, "attribute", 8)?));
        rows.push(Eff::new(
            "AT_ST_SP",
            vec![Some(int_field(attachment, "special")?)],
        ));
    }
    Ok(vec![rows])
}

const ATTACHMENTS: Family = Family {
    kind: "attachments",
    passes: &[&ATTACH],
    ignored: &[],
    build: build_attachments,
    flatten: flatten_attachments,
    ordered: true,
};

// --- ridepod_parts: ROBOINIT, RB_PARTS ----------------------------------------------------------

const ROBO: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "ROBOINIT",
            layout: "i",
        },
        // The slots after the offset depend on the part type, so they are kept as typed.
        Command {
            name: "RB_PARTS",
            layout: "i i i i *v",
        },
    ],
};

/// Reads typed rest slots of an `RB_PARTS` row as integers, failing on any other operand type.
fn part_int(row: &Eff, slot: usize) -> CoreResult<Value> {
    match row.vals.get(slot) {
        Some(Some(Val::I(v))) => Ok(json!(v)),
        _ => Err(fail(format!("RB_PARTS slot {slot} is not an integer"))),
    }
}

/// Reads the optional trailing label of an `RB_PARTS` row, which retail reads and discards.
fn part_label(row: &Eff, slot: usize) -> CoreResult<Option<Value>> {
    match row.vals.get(slot) {
        None => Ok(None),
        Some(Some(Val::S(text))) => Ok(Some(text_json(text))),
        _ => Err(fail(format!("RB_PARTS slot {slot} is not a label"))),
    }
}

/// Integer fields each part type reads after the offset, and whether a label follows them.
fn part_fields(part_type: i32) -> (&'static [&'static str], bool) {
    match part_type {
        0 => (&["defence"], true),
        1 => (
            &[
                "durability",
                "attack",
                "durable",
                "attribute",
                "attack_type",
            ],
            true,
        ),
        2 => (&["unk_4", "move_type"], false),
        3 => (&["energy"], false),
        _ => (&[], false),
    }
}

fn build_parts(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "ROBOINIT")?;
    let mut parts = Vec::new();
    while let Some(row) = cursor.take("RB_PARTS") {
        let part_type = row.int(1)?;
        let mut obj = Obj::new()
            .put("item_no", json!(row.int(0)?))
            .put("type", json!(part_type))
            .put("use_capacity", json!(row.int(2)?))
            .put("offset_no", json!(row.int(3)?));
        let (fields, labelled) = part_fields(part_type);
        let mut slot = 4;
        for key in fields {
            if *key == "attribute" {
                let values = (slot..slot + 8)
                    .map(|s| part_int(row, s))
                    .collect::<CoreResult<Vec<_>>>()?;
                obj = obj.put(key, Value::Array(values));
                slot += 8;
            } else {
                obj = obj.put(key, part_int(row, slot)?);
                slot += 1;
            }
        }
        if labelled {
            obj = obj.opt("label", part_label(row, slot)?);
            if row.count() > slot {
                slot += 1;
            }
        }
        if row.count() != slot {
            return Err(fail(format!(
                "RB_PARTS of type {part_type} has {} operands",
                row.count()
            )));
        }
        parts.push(obj.done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "parts": parts}))
}

fn flatten_parts(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "ROBOINIT")?];
    for part in array(document, "parts")? {
        let part_type = i32_of(field(part, "type")?)?;
        let mut vals = vec![
            Some(int_field(part, "item_no")?),
            Some(Val::I(part_type)),
            Some(int_field(part, "use_capacity")?),
            Some(int_field(part, "offset_no")?),
        ];
        let (fields, labelled) = part_fields(part_type);
        for key in fields {
            if *key == "attribute" {
                vals.extend(ints_of(part, key, 8)?);
            } else {
                vals.push(Some(int_field(part, key)?));
            }
        }
        if labelled && let Some(label) = part.get("label") {
            vals.push(Some(Val::S(text_of_json(label)?)));
        }
        rows.push(Eff::new("RB_PARTS", vals));
    }
    Ok(vec![rows])
}

const RIDEPOD_PARTS: Family = Family {
    kind: "ridepod_parts",
    passes: &[&ROBO],
    ignored: &[],
    build: build_parts,
    flatten: flatten_parts,
    ordered: true,
};

// --- fish: FISHINIT, FISH ----------------------------------------------------------------------

const FISH: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "FISHINIT",
            layout: "i",
        },
        Command {
            name: "FISH",
            layout: "i f i i i i i i i?",
        },
    ],
};

const FISH_KEYS: &[(&str, char)] = &[
    ("item_no", 'i'),
    ("size", 'f'),
    ("unk_4", 'i'),
    ("battle", 'i'),
    ("boost", 'i'),
    ("endurance", 'i'),
    ("tenacity", 'i'),
    ("stamina", 'i'),
    ("unk_10", 'i'),
];

fn build_fish(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "FISHINIT")?;
    let keys = keys_of(FISH_KEYS);
    let mut fish = Vec::new();
    while let Some(row) = cursor.take("FISH") {
        fish.push(record(row, &keys));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "fish": fish}))
}

fn flatten_fish(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "FISHINIT")?];
    for entry in array(document, "fish")? {
        rows.push(row_of(entry, "FISH", FISH_KEYS)?);
    }
    Ok(vec![rows])
}

const FISH_TABLE: Family = Family {
    kind: "fish",
    passes: &[&FISH],
    ignored: &[],
    build: build_fish,
    flatten: flatten_fish,
    ordered: true,
};

// --- guards: GRDNUM, GRD -----------------------------------------------------------------------

const GUARD: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "GRDNUM",
            layout: "i",
        },
        // Retail reads five more integers after the value and discards them.
        Command {
            name: "GRD",
            layout: "i i",
        },
    ],
};

fn build_guards(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = required_count(&mut cursor, "GRDNUM")?;
    let mut guards = Vec::new();
    while let Some(row) = cursor.take("GRD") {
        guards.push(json!({"item_no": row.int(0)?, "value": row.int(1)?}));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "guards": guards}))
}

fn flatten_guards(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![count_row(document, "GRDNUM")?];
    for guard in array(document, "guards")? {
        rows.push(row_of(guard, "GRD", &[("item_no", 'i'), ("value", 'i')])?);
    }
    Ok(vec![rows])
}

const GUARDS: Family = Family {
    kind: "guards",
    passes: &[&GUARD],
    ignored: &[],
    build: build_guards,
    flatten: flatten_guards,
    ordered: true,
};

// --- item_messages: MES_SYS (MES_SYSSPE reads its operands and discards them) -------------------

const MESSAGE: Pass = Pass {
    name: "gamedata_tag",
    commands: &[
        Command {
            name: "MES_SYS",
            layout: "i s",
        },
        Command {
            name: "MES_SYSSPE",
            layout: "-",
        },
    ],
};

fn build_messages(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let messages = passes[0]
        .iter()
        .map(|row| Ok(json!({"item_no": row.int(0)?, "name": text_json(&row.text(1)?)})))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(json!({"messages": messages}))
}

fn flatten_messages(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "messages")?
        .iter()
        .map(|message| row_of(message, "MES_SYS", &[("item_no", 'i'), ("name", 's')]))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(vec![rows])
}

const ITEM_MESSAGES: Family = Family {
    kind: "item_messages",
    passes: &[&MESSAGE],
    ignored: &[],
    build: build_messages,
    flatten: flatten_messages,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[
    &ITEM_COMMON,
    &WEAPONS,
    &ITEMS,
    &ATTACHMENTS,
    &RIDEPOD_PARTS,
    &FISH_TABLE,
    &GUARDS,
    &ITEM_MESSAGES,
];

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    let directory = path.strip_suffix(leaf)?;
    if !matches!(directory, "menu/cfg5/" | "menu/cfg6/" | "menu/cfg7/") {
        return None;
    }
    let stem = leaf.strip_suffix(".cfg")?;
    Some(match stem {
        "comdat" | "_comdat" => "item_common",
        "wepdat" => "weapons",
        "itemdat" => "items",
        "atdat" => "attachments",
        "robodat" => "ridepod_parts",
        "fishdat" => "fish",
        "grddat" => "guards",
        _ => {
            let language = stem.strip_prefix("comdatmes")?;
            if language.is_empty() || !language.bytes().all(|b| b.is_ascii_digit()) {
                return None;
            }
            "item_messages"
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> CoreResult<Value> {
        let document = super::super::super::configs::parse(script)?;
        super::super::build(alias, &document).map(|(native, _)| native)
    }

    #[test]
    fn tables_are_routed_by_file_name() {
        assert_eq!(
            kind("menu/cfg7/comdat.cfg", "comdat.cfg"),
            Some("item_common")
        );
        assert_eq!(
            kind("menu/cfg5/_comdat.cfg", "_comdat.cfg"),
            Some("item_common")
        );
        assert_eq!(
            kind("menu/cfg7/comdatmes3.cfg", "comdatmes3.cfg"),
            Some("item_messages")
        );
        assert_eq!(
            kind("menu/cfg7/robodat.cfg", "robodat.cfg"),
            Some("ridepod_parts")
        );
        assert_eq!(kind("menu/cfg7/commentmes.cfg", "commentmes.cfg"), None);
        assert_eq!(kind("menu/comdat.cfg", "comdat.cfg"), None);
    }

    #[test]
    fn common_records_keep_every_field() {
        let doc = convert(
            "menu/cfg7/comdat.cfg",
            b"COMINIT 2; COM 11,1,2,1,3,99,4,5,6,\"sword\",7; COM 12,1,3,0,1,1,4,5,6,x,0;",
        )
        .unwrap();
        assert_eq!(doc["kind"], "item_common");
        assert_eq!(doc["count"], 2);
        assert_eq!(doc["records"][0]["file_name"], "sword");
        assert_eq!(doc["records"][0]["max_num"], 99);
        assert_eq!(doc["records"][1]["file_name"], Value::Null);
        let doc = convert("menu/cfg7/comdat.cfg", b"COM 11,1,2,1,3,99,4,5,6,\"a\",7;").unwrap();
        assert!(doc.get("count").is_none());
        assert!(
            convert(
                "menu/cfg7/comdat.cfg",
                b"COM 11,1,2,1,3,99,4,5,6,\"a\",7; COMINIT 1;"
            )
            .is_err()
        );
    }

    #[test]
    fn weapons_group_their_rows_and_keep_optional_slots() {
        let doc = convert(
            "menu/cfg7/wepdat.cfg",
            b"WEPNUM 2;\
              WEP 36,32; WEP_ST 8,10; WEP_ST_L 20,19; WEP_ST2 1,2,3,4,5,6,7,8; WEP_ST2_L 9,9,9,9,9,9,9,9;\
              WEP_SPE 1.5,1,0,3,16; WEP_BUILD 2,90,0,-1,-1,-1;\
              WEP 1,2; WEP_ST 3,4; WEP_ST_L 5,6; WEP_ST2 0,0,0,0,0,0,0,0; WEP_ST2_L 0,0,0,0,0,0,0,0;\
              WEP_SPE 2,1,0,3,0,4,5; WEP_BUILD 7,8,9;",
        )
        .unwrap();
        assert_eq!(doc["count"], 2);
        let first = &doc["weapons"][0];
        assert_eq!(first["durability"], 36);
        assert_eq!(first["attribute"], json!([1, 2, 3, 4, 5, 6, 7, 8]));
        assert_eq!(first["special"]["initial_fusion_point"], 1.5);
        assert!(first["special"].get("attack_type").is_none());
        assert_eq!(first["buildup_monster"], json!([-1, -1, -1]));
        let second = &doc["weapons"][1];
        assert_eq!(second["special"]["attack_type"], 4);
        assert_eq!(second["special"]["model_no"], 5);
        assert!(second.get("buildup_monster").is_none());
        assert!(
            convert(
                "menu/cfg7/wepdat.cfg",
                b"WEPNUM 1; WEP 1,2; WEP_SPE 1,1,1,1,1;"
            )
            .is_err()
        );
    }

    #[test]
    fn short_weapon_buildup_reads_the_previous_slots() {
        // Four operands read the fifth and sixth monster slots left by the previous command.
        let doc = convert(
            "menu/cfg7/wepdat.cfg",
            b"WEPNUM 1; WEP 1,2; WEP_ST 3,4; WEP_ST_L 5,6; WEP_ST2 1,2,3,4,5,6,7,8;\
              WEP_ST2_L 0,0,0,0,0,0,0,0; WEP_SPE 2,1,0,3,0,4,5; WEP_BUILD 7,8,9,10;",
        )
        .unwrap();
        assert_eq!(doc["weapons"][0]["buildup_monster"], json!([10, 0, 4]));
    }

    #[test]
    fn items_attachments_fish_and_guards() {
        let doc = convert(
            "menu/cfg7/itemdat.cfg",
            b"ITEMINIT 1; ITEM 172,1,2,3,4,5,6;",
        )
        .unwrap();
        assert_eq!(
            doc["items"][0],
            json!({"item_no":172,"use_flags":1,"action_flags":2,"target_flags":3,"value":[4,5,6]})
        );
        let doc = convert(
            "menu/cfg7/atdat.cfg",
            b"AT_INIT 1; AT_ST 17,23; AT_ST2 1,2,3,4,5,6,7,8; AT_ST_SP 4660;",
        )
        .unwrap();
        assert_eq!(doc["attachments"][0]["special"], 4660);
        let doc = convert(
            "menu/cfg7/fishdat.cfg",
            b"FISHINIT 2; FISH 310,1450,54,6,12,8,8,7; FISH 311,2.5,1,2,3,4,5,6,9;",
        )
        .unwrap();
        assert_eq!(doc["fish"][0]["size"], 1450.0);
        assert!(doc["fish"][0].get("unk_10").is_none());
        assert_eq!(doc["fish"][1]["unk_10"], 9);
        let doc = convert(
            "menu/cfg7/grddat.cfg",
            b"GRDNUM 1; GRD 111,3,2,1,1,1,1,1,1;",
        )
        .unwrap();
        assert_eq!(doc["guards"][0], json!({"item_no":111,"value":3}));
    }

    #[test]
    fn ridepod_parts_keep_their_type_fields() {
        let doc = convert(
            "menu/cfg7/robodat.cfg",
            b"ROBOINIT 4; RB_PARTS 1,0,2,3,4,\"body\";\
              RB_PARTS 2,1,2,3,10,11,12,1,2,3,4,5,6,7,8,9,\"arm\";\
              RB_PARTS 3,2,2,3,5,6; RB_PARTS 4,3,2,3,7;",
        )
        .unwrap();
        let parts = &doc["parts"];
        assert_eq!(parts[0]["defence"], 4);
        assert_eq!(parts[0]["label"], "body");
        assert_eq!(parts[1]["attribute"], json!([1, 2, 3, 4, 5, 6, 7, 8]));
        assert_eq!(parts[1]["attack_type"], 9);
        assert_eq!(parts[2]["move_type"], 6);
        assert_eq!(parts[3]["energy"], 7);
        assert!(convert("menu/cfg7/robodat.cfg", b"ROBOINIT 1; RB_PARTS 4,3,2,3;").is_err());
        assert!(
            convert(
                "menu/cfg7/robodat.cfg",
                b"ROBOINIT 1; RB_PARTS 4,3,2,3,1,2;"
            )
            .is_err()
        );
    }

    #[test]
    fn item_messages_drop_spectol_rows() {
        let doc = convert(
            "menu/cfg7/comdatmes0.cfg",
            b"MES_SYS 11,\"Dagger\"; MES_SYSSPE 1,\"x\"; MES_SYS 12,x;",
        )
        .unwrap();
        assert_eq!(
            doc["messages"],
            json!([{"item_no":11,"name":"Dagger"},{"item_no":12,"name":null}])
        );
        assert!(convert("menu/cfg7/comdatmes0.cfg", b"COM 1;").is_err());
    }
}
