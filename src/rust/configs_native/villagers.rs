//! Party characters (`npc?.cfg`), villager places (`place.cfg`) and villager schedules
//! (`npc_place*.cfg`).
use super::*;

// --- party_characters: LoadNPCCfg (npccfg.cpp) ---------------------------------------------------

const NPC: Pass = Pass {
    name: "npc_spitag",
    commands: &[
        // The declared count is overwritten by the number of records once the script ends.
        Command {
            name: "NPC_NUM",
            layout: "-",
        },
        Command {
            name: "NPC_INFO",
            layout: "i s s i i i i i i i i",
        },
    ],
};

fn build_characters(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut characters = Vec::new();
    for row in &passes[0] {
        characters.push(
            Obj::new()
                .put("id", json!(row.int(0)?))
                .put("name", text_json(&row.text(1)?))
                .put("model", text_json(&row.text(2)?))
                .put("unk_31", json!(row.int(3)?))
                .put("ability_num", json!(row.int(4)?))
                .put("max_npc_point", json!(row.int(5)?))
                .put("ability_cost", ints_json(row, 6, 4)?)
                .put("debug_flag", json!(row.int(10)?))
                .done(),
        );
    }
    Ok(json!({ "characters": characters }))
}

fn flatten_characters(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for character in array(document, "characters")? {
        let mut vals = vec![
            Some(int_field(character, "id")?),
            Some(text_field(character, "name")?),
            Some(text_field(character, "model")?),
            Some(int_field(character, "unk_31")?),
            Some(int_field(character, "ability_num")?),
            Some(int_field(character, "max_npc_point")?),
        ];
        vals.extend(int_slots(character, "ability_cost", 4)?);
        vals.push(Some(int_field(character, "debug_flag")?));
        rows.push(Eff::new("NPC_INFO", vals));
    }
    Ok(vec![rows])
}

const PARTY_CHARACTERS: Family = Family {
    kind: "party_characters",
    passes: &[&NPC],
    ignored: &[],
    build: build_characters,
    flatten: flatten_characters,
    ordered: true,
};

// --- villager_places: LoadPlaceInfo (vlgr_info.cpp) ----------------------------------------------

const PLACE: Pass = Pass {
    name: "tag",
    commands: &[
        Command {
            name: "NPC_PLACE_NUM",
            layout: "i",
        },
        Command {
            name: "NPC_PLACE",
            layout: "i i",
        },
        Command {
            name: "NPC_PLACE_END",
            layout: "",
        },
        Command {
            name: "PLACE_POS",
            layout: "f f f f",
        },
        Command {
            name: "MOTION",
            layout: "s",
        },
        Command {
            name: "MOVE_TO",
            layout: "f f f f",
        },
        Command {
            name: "WAIT",
            layout: "s i s?",
        },
        Command {
            name: "TALK_OFFSET",
            layout: "f f f",
        },
        Command {
            name: "MOVE_MOTION",
            layout: "s",
        },
        Command {
            name: "MOVE_SPEED",
            layout: "f",
        },
        Command {
            name: "SHADOW",
            layout: "i",
        },
    ],
};

/// Place settings each set once per place block, with their document keys.
const PLACE_SETTINGS: &[&str] = &[
    "PLACE_POS",
    "TALK_OFFSET",
    "MOTION",
    "MOVE_MOTION",
    "MOVE_SPEED",
    "SHADOW",
];

fn build_place_block(cursor: &mut Cursor) -> CoreResult<Value> {
    let head = cursor.expect("NPC_PLACE")?;
    let mut place = Obj::new()
        .put("index", json!(head.int(0)?))
        .put("map", json!(head.int(1)?));
    let mut seen = Vec::new();
    let mut route = Vec::new();
    loop {
        let Some(row) = cursor.next() else {
            return Err(fail("NPC_PLACE without NPC_PLACE_END"));
        };
        let name = row.name.as_str();
        if PLACE_SETTINGS.contains(&name) {
            if seen.contains(&name) {
                return Err(fail(format!("{name} repeated within one NPC_PLACE")));
            }
            seen.push(name);
        }
        place = match name {
            "NPC_PLACE_END" => break,
            "PLACE_POS" => place
                .put("position", floats_json(row, 0, 3)?)
                .put("angle", float_json(row.float(3)?)),
            "TALK_OFFSET" => place.put("talk_offset", floats_json(row, 0, 3)?),
            "MOTION" => place.put("motion", text_json(&row.text(0)?)),
            "MOVE_MOTION" => place.put("move_motion", text_json(&row.text(0)?)),
            "MOVE_SPEED" => place.put("move_speed", float_json(row.float(0)?)),
            "SHADOW" => place.put("shadow", json!(row.int(0)?)),
            "MOVE_TO" => {
                route.push(
                    Obj::new()
                        .put("type", json!("move"))
                        .put("position", floats_json(row, 0, 3)?)
                        .put("angle", float_json(row.float(3)?))
                        .done(),
                );
                place
            }
            "WAIT" => {
                route.push(
                    Obj::new()
                        .put("type", json!("wait"))
                        .put("posture", text_json(&row.text(0)?))
                        .put("time", json!(row.int(1)?))
                        .opt("motion", slot_json(row, 2))
                        .done(),
                );
                place
            }
            other => return Err(fail(format!("{other} inside NPC_PLACE"))),
        };
    }
    Ok(place.put("route", Value::Array(route)).done())
}

fn build_places(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    // Retail ignores place settings outside a place block; the corpus has none, so any is refused.
    let count = cursor.expect("NPC_PLACE_NUM")?.int(0)?;
    let mut places = Vec::new();
    while !cursor.done() {
        if cursor.peek_name() != Some("NPC_PLACE") {
            return Err(cursor.unexpected());
        }
        places.push(build_place_block(&mut cursor)?);
    }
    Ok(json!({"place_count": count, "places": places}))
}

fn flatten_places(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new(
        "NPC_PLACE_NUM",
        vec![Some(int_field(document, "place_count")?)],
    )];
    for place in array(document, "places")? {
        rows.push(Eff::new(
            "NPC_PLACE",
            vec![
                Some(int_field(place, "index")?),
                Some(int_field(place, "map")?),
            ],
        ));
        if place.get("position").is_some() {
            let mut vals = float_slots(place, "position", 3)?;
            vals.push(Some(float_field(place, "angle")?));
            rows.push(Eff::new("PLACE_POS", vals));
        }
        if place.get("talk_offset").is_some() {
            rows.push(Eff::new(
                "TALK_OFFSET",
                float_slots(place, "talk_offset", 3)?,
            ));
        }
        if let Some(motion) = place.get("motion") {
            rows.push(Eff::new("MOTION", vec![Some(text_val(motion)?)]));
        }
        if let Some(motion) = place.get("move_motion") {
            rows.push(Eff::new("MOVE_MOTION", vec![Some(text_val(motion)?)]));
        }
        if let Some(speed) = place.get("move_speed") {
            rows.push(Eff::new("MOVE_SPEED", vec![Some(float_val(speed)?)]));
        }
        if let Some(shadow) = place.get("shadow") {
            rows.push(Eff::new("SHADOW", vec![Some(int_val(shadow)?)]));
        }
        for node in array(place, "route")? {
            match field(node, "type")?.as_str() {
                Some("move") => {
                    let mut vals = float_slots(node, "position", 3)?;
                    vals.push(Some(float_field(node, "angle")?));
                    rows.push(Eff::new("MOVE_TO", vals));
                }
                Some("wait") => rows.push(Eff::new(
                    "WAIT",
                    vec![
                        Some(text_field(node, "posture")?),
                        Some(int_field(node, "time")?),
                        opt_field(node, "motion", text_val)?,
                    ],
                )),
                _ => return Err(fail("unknown route step type")),
            }
        }
        rows.push(Eff::new("NPC_PLACE_END", Vec::new()));
    }
    Ok(vec![rows])
}

const VILLAGER_PLACES: Family = Family {
    kind: "villager_places",
    passes: &[&PLACE],
    ignored: &[],
    build: build_places,
    flatten: flatten_places,
    // Settings of one place are independent of their order; the route keeps its own order.
    ordered: false,
};

// --- villager_schedule: LoadGameInfo's gi_tag pass, then LoadNPCInfo's ni_tag pass ----------------

const GAME_INFO: Pass = Pass {
    name: "gi_tag",
    commands: &[Command {
        name: "PROG_INFO",
        layout: "i i i i s?4",
    }],
};

const NPC_INFO: Pass = Pass {
    name: "ni_tag",
    commands: &[
        Command {
            name: "NPC",
            layout: "i",
        },
        Command {
            name: "NPC_END",
            layout: "",
        },
        Command {
            name: "PROGRESS",
            layout: "i i s?",
        },
        Command {
            name: "PROGRESS_END",
            layout: "",
        },
        Command {
            name: "PLACE",
            layout: "-",
        },
        Command {
            name: "NOON_PLACE",
            layout: "i",
        },
        Command {
            name: "NIGHT_PLACE",
            layout: "i",
        },
        Command {
            name: "NPC_INFO_NUM",
            layout: "i",
        },
        Command {
            name: "NPC_INFO",
            layout: "i s i? s? s? i?6 i?6",
        },
    ],
};

/// Document keys of a PROG_INFO row, by slot.
const PROGRESS_INFO_KEYS: &[(&str, char)] = &[
    ("progress", 'i'),
    ("chapter", 'i'),
    ("section", 'i'),
    ("order", 'i'),
    ("name", 's'),
];

/// Document keys of an NPC_INFO row, by slot.
const VILLAGER_INFO_KEYS: &[(&str, char)] = &[
    ("id", 'i'),
    ("model", 's'),
    ("house_type", 'i'),
    ("show_frames", 's'),
    ("hide_frames", 's'),
    ("unk_14", 'i'),
    ("unk_18", 'i'),
];

fn keys_of(keys: &[(&'static str, char)]) -> Vec<&'static str> {
    keys.iter().map(|(key, _)| *key).collect()
}

fn build_progress_info(rows: &[Eff]) -> CoreResult<Vec<Value>> {
    let keys = keys_of(PROGRESS_INFO_KEYS);
    let mut named = false;
    let mut out = Vec::new();
    for row in rows {
        // A short row's name slot was never written in the corpus, which the guard reproduces; a
        // name left there by an earlier row would be a stale string.
        if !row.has(4) && named {
            return Err(fail("PROG_INFO without a name after a named PROG_INFO"));
        }
        named |= row.has(4);
        out.push(row_json(row, &keys));
    }
    Ok(out)
}

fn build_villager_block(cursor: &mut Cursor, condition_written: &mut bool) -> CoreResult<Value> {
    let id = cursor.expect("NPC")?.int(0)?;
    let mut progress = Vec::new();
    while cursor.take("NPC_END").is_none() {
        let row = cursor.expect("PROGRESS")?;
        if !row.has(2) && *condition_written {
            return Err(fail(
                "PROGRESS without a condition after one with a condition",
            ));
        }
        *condition_written |= row.has(2);
        let mut entry = Obj::new()
            .put("progress", json!(row.int(0)?))
            .put("duplicate_id", json!(row.int(1)?))
            .opt("condition", slot_json(row, 2));
        if let Some(noon) = cursor.take("NOON_PLACE") {
            entry = entry.put("noon_place", json!(noon.int(0)?));
        }
        if let Some(night) = cursor.take("NIGHT_PLACE") {
            entry = entry.put("night_place", json!(night.int(0)?));
        }
        cursor.expect("PROGRESS_END")?;
        progress.push(entry.done());
    }
    Ok(json!({"id": id, "progress": progress}))
}

fn build_schedule(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut body = Obj::new().put(
        "progress_info",
        Value::Array(build_progress_info(&passes[0])?),
    );
    let mut cursor = Cursor::new(&passes[1]);
    let mut condition_written = false;
    let mut villagers = Vec::new();
    while cursor.peek_name() == Some("NPC") {
        villagers.push(build_villager_block(&mut cursor, &mut condition_written)?);
    }
    body = body.put("villagers", Value::Array(villagers));
    if let Some(count) = cursor.take("NPC_INFO_NUM") {
        let keys = keys_of(VILLAGER_INFO_KEYS);
        let mut entries = Vec::new();
        while let Some(row) = cursor.take("NPC_INFO") {
            entries.push(row_json(row, &keys));
        }
        body = body.put(
            "villager_info",
            json!({"count": count.int(0)?, "villagers": entries}),
        );
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(body.done())
}

fn flatten_schedule(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut game = Vec::new();
    for info in array(document, "progress_info")? {
        game.push(row_of(info, "PROG_INFO", PROGRESS_INFO_KEYS)?);
    }
    let mut npc = Vec::new();
    for villager in array(document, "villagers")? {
        npc.push(Eff::new("NPC", vec![Some(int_field(villager, "id")?)]));
        for entry in array(villager, "progress")? {
            npc.push(Eff::new(
                "PROGRESS",
                vec![
                    Some(int_field(entry, "progress")?),
                    Some(int_field(entry, "duplicate_id")?),
                    opt_field(entry, "condition", text_val)?,
                ],
            ));
            if let Some(place) = entry.get("noon_place") {
                npc.push(Eff::new("NOON_PLACE", vec![Some(int_val(place)?)]));
            }
            if let Some(place) = entry.get("night_place") {
                npc.push(Eff::new("NIGHT_PLACE", vec![Some(int_val(place)?)]));
            }
            npc.push(Eff::new("PROGRESS_END", Vec::new()));
        }
        npc.push(Eff::new("NPC_END", Vec::new()));
    }
    if let Some(info) = document.get("villager_info") {
        npc.push(Eff::new(
            "NPC_INFO_NUM",
            vec![Some(int_field(info, "count")?)],
        ));
        for entry in array(info, "villagers")? {
            npc.push(row_of(entry, "NPC_INFO", VILLAGER_INFO_KEYS)?);
        }
    }
    Ok(vec![game, npc])
}

const VILLAGER_SCHEDULE: Family = Family {
    kind: "villager_schedule",
    passes: &[&GAME_INFO, &NPC_INFO],
    ignored: &[],
    build: build_schedule,
    flatten: flatten_schedule,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[&PARTY_CHARACTERS, &VILLAGER_PLACES, &VILLAGER_SCHEDULE];

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    if path != leaf {
        return None;
    }
    let stem = leaf.strip_suffix(".cfg")?;
    if let Some(language) = stem.strip_prefix("npc_place") {
        return (language.is_empty() || language.bytes().all(|b| b.is_ascii_digit()))
            .then_some("villager_schedule");
    }
    if let Some(language) = stem.strip_prefix("npc") {
        return (language.len() <= 1 && language.bytes().all(|b| b.is_ascii_digit()))
            .then_some("party_characters");
    }
    (stem == "place").then_some("villager_places")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Encodes a test script as the game's CP932 bytes.
    fn cp932(script: &str) -> Vec<u8> {
        let mut out = Vec::new();
        for c in script.chars() {
            match c {
                '以' => out.extend([0x88, 0xC8]),
                '後' => out.extend([0x8C, 0xE3]),
                '中' => out.extend([0x92, 0x86]),
                '昼' => out.extend([0x92, 0x8B]),
                c if c.is_ascii() => out.push(c as u8),
                _ => panic!("no test encoding for {c}"),
            }
        }
        out
    }

    fn convert(alias: &str, script: &[u8]) -> CoreResult<Value> {
        let document = super::super::super::configs::parse(script)?;
        super::super::build(alias, &document).map(|(native, _)| native)
    }

    #[test]
    fn paths_choose_the_villager_families() {
        assert_eq!(kind("npc.cfg", "npc.cfg"), Some("party_characters"));
        assert_eq!(kind("npc3.cfg", "npc3.cfg"), Some("party_characters"));
        assert_eq!(kind("place.cfg", "place.cfg"), Some("villager_places"));
        assert_eq!(
            kind("npc_place.cfg", "npc_place.cfg"),
            Some("villager_schedule")
        );
        assert_eq!(
            kind("npc_place4.cfg", "npc_place4.cfg"),
            Some("villager_schedule")
        );
        assert_eq!(kind("menu/0/npcmove.pac/npcmove.cfg", "npcmove.cfg"), None);
        assert_eq!(
            kind("dungeon/cfg_file/mos_place0.cfg", "mos_place0.cfg"),
            None
        );
        assert_eq!(kind("npcmove.cfg", "npcmove.cfg"), None);
    }

    #[test]
    fn characters_keep_every_record_and_drop_the_declared_count() {
        let doc = convert(
            "npc1.cfg",
            "NPC_NUM 99; NPC_INFO 7,\"First\",\"npc_first\",3,2,25,1,2,3,4,1,77; NPC_INFO 8,x,\"m\",0,0,0,0,0,0,0,0;"
                .as_bytes(),
        )
        .unwrap();
        assert_eq!(doc["kind"], "party_characters");
        assert_eq!(
            doc["characters"][0],
            json!({"id":7,"name":"First","model":"npc_first","unk_31":3,"ability_num":2,
                   "max_npc_point":25,"ability_cost":[1,2,3,4],"debug_flag":1})
        );
        assert_eq!(doc["characters"][1]["name"], Value::Null);
        assert!(doc.get("count").is_none());
    }

    #[test]
    fn places_nest_settings_and_routes() {
        let doc = convert(
            "place.cfg",
            b"NPC_PLACE_NUM 2; NPC_PLACE 0,101; MOTION \"sit\"; PLACE_POS 1.25,2.5,-3,0.75; \
              TALK_OFFSET 4,5,6; MOVE_MOTION \"run\"; MOVE_SPEED 1.5; SHADOW 0; MOVE_TO 7,8,9,1; \
              WAIT \"mtn\",45,\"stand\"; WAIT \"cnt\",3; NPC_PLACE_END; NPC_PLACE 0,7; NPC_PLACE_END;",
        )
        .unwrap();
        assert_eq!(doc["place_count"], 2);
        let place = &doc["places"][0];
        assert_eq!(place["position"], json!([1.25, 2.5, -3.0]));
        assert_eq!(place["angle"], json!(0.75));
        assert_eq!(place["talk_offset"], json!([4.0, 5.0, 6.0]));
        assert_eq!(place["motion"], "sit");
        assert_eq!(place["move_motion"], "run");
        assert_eq!(place["move_speed"], json!(1.5));
        assert_eq!(place["shadow"], 0);
        assert_eq!(
            place["route"][0],
            json!({"type":"move","position":[7.0,8.0,9.0],"angle":1.0})
        );
        assert_eq!(
            place["route"][1],
            json!({"type":"wait","posture":"mtn","time":45,"motion":"stand"})
        );
        assert!(place["route"][2].get("motion").is_none());
        // A repeated index is a second block applied to the same place.
        assert_eq!(doc["places"][1], json!({"index":0,"map":7,"route":[]}));
    }

    #[test]
    fn places_refuse_settings_outside_a_block() {
        let error = convert("place.cfg", b"MOVE_SPEED 2; NPC_PLACE_NUM 1;").unwrap_err();
        assert!(
            error.to_string().contains("expected NPC_PLACE_NUM"),
            "{error}"
        );
        let error = convert(
            "place.cfg",
            b"NPC_PLACE_NUM 1; NPC_PLACE 0,1; NPC_PLACE_END; SHADOW 1;",
        )
        .unwrap_err();
        assert!(error.to_string().contains("unexpected SHADOW"), "{error}");
        let error = convert(
            "place.cfg",
            b"NPC_PLACE_NUM 1; NPC_PLACE 0,1; SHADOW 1; SHADOW 0; NPC_PLACE_END;",
        )
        .unwrap_err();
        assert!(error.to_string().contains("repeated"), "{error}");
    }

    #[test]
    fn schedules_keep_both_passes() {
        let doc = convert(
            "npc_place4.cfg",
            &cp932(
                "PROG_INFO -1,0,0,0,\"x\"; PROG_INFO 5,2,3,100,\"First\"; \
             NPC 7; PROGRESS 5,1,\"以後\"; NOON_PLACE 0; NIGHT_PLACE 1; PROGRESS_END; \
             PROGRESS 5,0,\"中\"; NIGHT_PLACE 2; PROGRESS_END; NPC_END; \
             NPC_INFO_NUM 2; NPC_INFO 7,\"m\",1,\"hat\",\"c\",11,12; NPC_INFO 8,\"n\",2,\"\",\"\";",
            ),
        )
        .unwrap();
        assert_eq!(doc["kind"], "villager_schedule");
        assert_eq!(doc["progress_info"][0]["progress"], -1);
        assert_eq!(
            doc["progress_info"][1],
            json!({"progress":5,"chapter":2,"section":3,"order":100,"name":"First"})
        );
        let villager = &doc["villagers"][0];
        assert_eq!(villager["id"], 7);
        assert_eq!(
            villager["progress"][0],
            json!({"progress":5,"duplicate_id":1,"condition":"以後","noon_place":0,"night_place":1})
        );
        assert_eq!(
            villager["progress"][1],
            json!({"progress":5,"duplicate_id":0,"condition":"中","night_place":2})
        );
        assert_eq!(doc["villager_info"]["count"], 2);
        assert_eq!(
            doc["villager_info"]["villagers"][0],
            json!({"id":7,"model":"m","house_type":1,"show_frames":"hat","hide_frames":"c","unk_14":11,"unk_18":12})
        );
        assert!(doc["villager_info"]["villagers"][1].get("unk_14").is_none());
        assert_eq!(doc["villager_info"]["villagers"][1]["show_frames"], "");
    }

    #[test]
    fn schedules_without_conditions_or_names_keep_them_absent() {
        let doc = convert(
            "npc_place.cfg",
            &cp932("PROG_INFO 1,0,0,0; NPC 1; PROGRESS 1,\"昼\"; PLACE 3; PROGRESS_END; NPC_END;"),
        )
        .unwrap();
        assert!(doc["progress_info"][0].get("name").is_none());
        assert_eq!(
            doc["villagers"][0]["progress"][0],
            json!({"progress":1,"duplicate_id":0})
        );
        assert!(doc.get("villager_info").is_none());
        // A short row after a full one would read the earlier row's string.
        let error = convert(
            "npc_place.cfg",
            &cp932(
                "NPC 1; PROGRESS 1,0,\"中\"; PROGRESS_END; PROGRESS 2,0; PROGRESS_END; NPC_END;",
            ),
        )
        .unwrap_err();
        assert!(error.to_string().contains("without a condition"), "{error}");
    }

    #[test]
    fn schedules_refuse_unknown_shapes() {
        let error = convert("npc_place4.cfg", b"NPC 1; NOON_PLACE 1; NPC_END;").unwrap_err();
        assert!(error.to_string().contains("expected PROGRESS"), "{error}");
        let error = convert(
            "npc_place4.cfg",
            b"NPC_INFO_NUM 1; NPC_INFO 1,\"m\"; NPC 1; NPC_END;",
        )
        .unwrap_err();
        assert!(error.to_string().contains("unexpected NPC"), "{error}");
    }
}
