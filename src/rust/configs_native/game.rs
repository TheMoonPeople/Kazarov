//! Game-wide scripts: the game setup actions of `LoadGameConfig` (mainloop.cpp), the map names of
//! `LoadMapName` (mapselect.cpp) and the movie list of the movie viewer (movieviewlp.cpp).
use super::*;

// --- game_setup: LoadGameConfig (tag__3) -------------------------------------------------------

/// Slots `gcGET_N_ITEM` reads: two per argument, so slot `k` is read when the argument count
/// exceeds `k / 2`; the second half of the pairs comes from slots the previous commands left.
const GET_N_ITEM_LAYOUT: &str = "i?0 i?0 i?1 i?1 i?2 i?2 i?3 i?3 i?4 i?4 i?5 i?5 i?6 i?6 i?7 i?7 \
     i?8 i?8 i?9 i?9 i?10 i?10 i?11 i?11 i?12 i?12 i?13 i?13 i?14 i?14 i?15 i?15 \
     i?16 i?16 i?17 i?17 i?18 i?18 i?19 i?19 i?20 i?20 i?21 i?21 i?22 i?22 i?23 i?23 \
     i?24 i?24 i?25 i?25 i?26 i?26 i?27 i?27 i?28 i?28 i?29 i?29 i?30 i?30 i?31 i?31";

/// Pairs the 64 slots of the retail stack hold.
const GET_N_ITEM_PAIRS: usize = 32;

const SETUP: Pass = Pass {
    name: "tag__3",
    commands: &[
        Command {
            name: "MAP_NO",
            layout: "v",
        },
        Command {
            name: "PROGRESS",
            layout: "i",
        },
        Command {
            name: "BIT_FLAG_ON",
            layout: "*i",
        },
        Command {
            name: "BIT_FLAG_OFF",
            layout: "*i",
        },
        Command {
            name: "START_EVENT",
            layout: "i",
        },
        Command {
            name: "GEO_COMPLETE",
            layout: "*i",
        },
        Command {
            name: "GEO_DEBUG",
            layout: "",
        },
        Command {
            name: "ITEM_SET",
            layout: "i",
        },
        Command {
            name: "GET_ITEM",
            layout: "*i",
        },
        Command {
            name: "GET_N_ITEM",
            layout: GET_N_ITEM_LAYOUT,
        },
        Command {
            name: "EQUIP",
            layout: "i i",
        },
        Command {
            name: "DEFENSE",
            layout: "i i",
        },
        Command {
            name: "DEFENCE",
            layout: "i i",
        },
        Command {
            name: "HP",
            layout: "i i",
        },
        Command {
            name: "ALL_GEO_PARTS",
            layout: "i?",
        },
        Command {
            name: "PARAM_DRAW",
            layout: "i",
        },
        Command {
            name: "OPTION",
            layout: "s i",
        },
        Command {
            name: "MONICA",
            layout: "",
        },
        // The core is read only with exactly two operands; the second slot tells three or more apart.
        Command {
            name: "STEVE",
            layout: "i?1 i?2",
        },
        Command {
            name: "MONSTER",
            layout: "*i",
        },
        Command {
            name: "PARTY",
            layout: "i",
        },
        Command {
            name: "ACTIVE_CHARA",
            layout: "i",
        },
    ],
};

/// Commands whose callbacks read one integer list, with the action and field names they become.
const LISTS: &[(&str, &str, &str)] = &[
    ("BIT_FLAG_ON", "bit_flags_on", "flags"),
    ("BIT_FLAG_OFF", "bit_flags_off", "flags"),
    ("GEO_COMPLETE", "georama_complete", "towns"),
    ("GET_ITEM", "get_items", "items"),
    ("MONSTER", "monsters", "ids"),
];

/// Commands whose callbacks read fixed integer slots, with their action and field names.
const RECORDS: &[(&str, &str, &[&str])] = &[
    ("PROGRESS", "progress", &["value"]),
    ("START_EVENT", "start_event", &["event"]),
    ("GEO_DEBUG", "georama_debug", &[]),
    ("ITEM_SET", "item_set", &["set"]),
    ("EQUIP", "equip", &["chara", "item"]),
    ("DEFENSE", "defense", &["chara", "value"]),
    ("DEFENCE", "defence", &["chara", "value"]),
    ("HP", "hp", &["chara", "value"]),
    ("ALL_GEO_PARTS", "all_georama_parts", &["mode"]),
    ("PARAM_DRAW", "param_draw", &["show"]),
    ("MONICA", "monica", &[]),
    ("PARTY", "party", &["chara"]),
    ("ACTIVE_CHARA", "active_chara", &["chara"]),
];

fn setup_action(row: &Eff) -> CoreResult<Value> {
    let name = row.name.as_str();
    if let Some((_, action, key)) = LISTS.iter().find(|(command, _, _)| *command == name) {
        return Ok(json!({"action": action, *key: ints_json(row, 0, row.count())?}));
    }
    if let Some((_, action, keys)) = RECORDS.iter().find(|(command, _, _)| *command == name) {
        let mut obj = Obj::new().put("action", json!(action));
        for (slot, key) in keys.iter().enumerate() {
            obj = obj.opt(key, slot_json(row, slot));
        }
        return Ok(obj.done());
    }
    Ok(match name {
        "MAP_NO" => {
            let map = row.vals[0]
                .as_ref()
                .ok_or_else(|| fail("MAP_NO without a map"))?;
            json!({"action": "map", "map": raw_json(map)})
        }
        "GET_N_ITEM" => {
            let mut items = Vec::new();
            for pair in 0..GET_N_ITEM_PAIRS {
                if !row.has(pair * 2) {
                    break;
                }
                items.push(json!({"item": row.int(pair * 2)?, "count": row.int(pair * 2 + 1)?}));
            }
            json!({"action": "get_items_counted", "items": items})
        }
        "OPTION" => json!({
            "action": "option",
            "name": text_json(&row.text(0)?),
            "value": row.int(1)?,
        }),
        "STEVE" => {
            if row.has(1) {
                return Err(fail("STEVE with more than two operands"));
            }
            Obj::new()
                .put("action", json!("steve"))
                .opt("core", slot_json(row, 0))
                .done()
        }
        _ => return Err(fail(format!("game setup command {name} has no action"))),
    })
}

fn build_setup(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let actions = passes[0]
        .iter()
        .map(setup_action)
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(json!({"actions": actions}))
}

fn setup_row(action: &Value) -> CoreResult<Eff> {
    let name = field(action, "action")?
        .as_str()
        .ok_or_else(|| fail("action is not a string"))?;
    if let Some((command, _, key)) = LISTS.iter().find(|(_, a, _)| *a == name) {
        let vals = array(action, key)?
            .iter()
            .map(|v| int_val(v).map(Some))
            .collect::<CoreResult<Vec<_>>>()?;
        return Ok(Eff::new(command, vals));
    }
    if let Some((command, _, keys)) = RECORDS.iter().find(|(_, a, _)| *a == name) {
        let vals = keys
            .iter()
            .map(|key| opt_field(action, key, int_val))
            .collect::<CoreResult<Vec<_>>>()?;
        return Ok(Eff::new(command, vals));
    }
    Ok(match name {
        "map" => Eff::new("MAP_NO", vec![Some(raw_of(field(action, "map")?)?)]),
        "get_items_counted" => {
            let items = array(action, "items")?;
            if items.len() > GET_N_ITEM_PAIRS {
                return Err(fail("get_items_counted holds more pairs than the stack"));
            }
            let mut vals = Vec::new();
            for item in items {
                vals.push(Some(int_field(item, "item")?));
                vals.push(Some(int_field(item, "count")?));
            }
            vals.resize(GET_N_ITEM_PAIRS * 2, None);
            Eff::new("GET_N_ITEM", vals)
        }
        "option" => Eff::new(
            "OPTION",
            vec![
                Some(text_field(action, "name")?),
                Some(int_field(action, "value")?),
            ],
        ),
        "steve" => Eff::new("STEVE", vec![opt_field(action, "core", int_val)?, None]),
        _ => return Err(fail(format!("unknown game setup action {name}"))),
    })
}

fn flatten_setup(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "actions")?
        .iter()
        .map(setup_row)
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(vec![rows])
}

const GAME_SETUP: Family = Family {
    kind: "game_setup",
    passes: &[&SETUP],
    // A misspelt GET_N_ITEM in a retail debug configuration.
    ignored: &["ET_N_ITEM"],
    build: build_setup,
    flatten: flatten_setup,
    ordered: true,
};

// --- map_names: LoadMapName (tag) --------------------------------------------------------------

const MAP_NAME: Pass = Pass {
    name: "tag",
    commands: &[
        Command {
            name: "MAP_NAME_NUM",
            layout: "i",
        },
        Command {
            name: "MAP_NAME",
            layout: "s s s i i i? i?",
        },
    ],
};

const MAP_NAME_KEYS: &[(&str, char)] = &[
    ("name", 's'),
    ("title", 's'),
    ("add_path", 's'),
    ("type", 'i'),
    ("sel_type", 'i'),
    ("snd_data_id", 'i'),
    ("area_no", 'i'),
];

fn build_map_names(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = cursor.expect("MAP_NAME_NUM")?.int(0)?;
    let keys: Vec<&str> = MAP_NAME_KEYS.iter().map(|(key, _)| *key).collect();
    let mut maps = Vec::new();
    while let Some(row) = cursor.take("MAP_NAME") {
        maps.push(row_json(row, &keys));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "maps": maps}))
}

fn flatten_map_names(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new(
        "MAP_NAME_NUM",
        vec![Some(int_field(document, "count")?)],
    )];
    for map in array(document, "maps")? {
        rows.push(row_of(map, "MAP_NAME", MAP_NAME_KEYS)?);
    }
    Ok(vec![rows])
}

const MAP_NAMES: Family = Family {
    kind: "map_names",
    passes: &[&MAP_NAME],
    ignored: &[],
    build: build_map_names,
    flatten: flatten_map_names,
    ordered: true,
};

// --- movie_list: MovieViewInit (tag_movie) -----------------------------------------------------

const MOVIE: Pass = Pass {
    name: "tag_movie",
    commands: &[Command {
        name: "MOVIE",
        layout: "s s i?",
    }],
};

const MOVIE_KEYS: &[(&str, char)] = &[("title", 's'), ("file", 's'), ("bgm", 'i')];

fn build_movies(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let movies: Vec<Value> = passes[0]
        .iter()
        .map(|row| row_json(row, &["title", "file", "bgm"]))
        .collect();
    Ok(json!({"movies": movies}))
}

fn flatten_movies(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "movies")?
        .iter()
        .map(|movie| row_of(movie, "MOVIE", MOVIE_KEYS))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(vec![rows])
}

const MOVIE_LIST: Family = Family {
    kind: "movie_list",
    passes: &[&MOVIE],
    ignored: &[],
    build: build_movies,
    flatten: flatten_movies,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[&GAME_SETUP, &MAP_NAMES, &MOVIE_LIST];

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    if path == "game.cfg"
        || (path.starts_with("dbg/") && path == format!("dbg/{leaf}") && leaf.ends_with(".cfg"))
    {
        return Some("game_setup");
    }
    if path == "mv.cfg" {
        return Some("movie_list");
    }
    // LoadMapName reads map/map<language>.cfg; map/map.cfg has no reader.
    if let Some(language) = path
        .strip_prefix("map/map")
        .and_then(|rest| rest.strip_suffix(".cfg"))
        && !language.is_empty()
        && language.bytes().all(|b| b.is_ascii_digit())
    {
        return Some("map_names");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> CoreResult<(Value, Report)> {
        let document = super::super::super::configs::parse(script)?;
        super::super::build(alias, &document)
    }

    #[test]
    fn routing() {
        assert_eq!(kind("game.cfg", "game.cfg"), Some("game_setup"));
        assert_eq!(kind("dbg/game0.cfg", "game0.cfg"), Some("game_setup"));
        assert_eq!(kind("map/map3.cfg", "map3.cfg"), Some("map_names"));
        assert_eq!(kind("map/map.cfg", "map.cfg"), None);
        assert_eq!(kind("mv.cfg", "mv.cfg"), Some("movie_list"));
    }

    #[test]
    fn game_setup_keeps_action_order_and_types() {
        let (doc, report) = convert(
            "dbg/game0.cfg",
            b"PROGRESS 2; MAP_NO \"s01\"; MAP_NO 12; BIT_FLAG_ON 1,2,3; GEO_DEBUG; \
              OPTION \"MonsterName\",1; STEVE 1; STEVE 1,4; ALL_GEO_PARTS; ALL_GEO_PARTS 1; \
              ET_N_ITEM 1,2; DEFENCE 1,2; MONICA;",
        )
        .unwrap();
        assert_eq!(doc["kind"], "game_setup");
        let actions = doc["actions"].as_array().unwrap();
        assert_eq!(actions[0], json!({"action":"progress","value":2}));
        assert_eq!(actions[1], json!({"action":"map","map":"s01"}));
        assert_eq!(actions[2], json!({"action":"map","map":12}));
        assert_eq!(actions[3], json!({"action":"bit_flags_on","flags":[1,2,3]}));
        assert_eq!(actions[4], json!({"action":"georama_debug"}));
        assert_eq!(
            actions[5],
            json!({"action":"option","name":"MonsterName","value":1})
        );
        assert_eq!(actions[6], json!({"action":"steve"}));
        assert_eq!(actions[7], json!({"action":"steve","core":1}));
        assert_eq!(actions[8], json!({"action":"all_georama_parts"}));
        assert_eq!(actions[9], json!({"action":"all_georama_parts","mode":1}));
        assert_eq!(actions[10], json!({"action":"defence","chara":1,"value":2}));
        assert_eq!(actions[11], json!({"action":"monica"}));
        assert_eq!(report.dropped, vec!["ET_N_ITEM".to_string()]);
        assert!(convert("dbg/game0.cfg", b"STEVE 1,2,3;").is_err());
    }

    #[test]
    fn counted_items_read_two_slots_per_operand() {
        // Two operands make two pairs: the second pair comes from the slots GET_ITEM left.
        let (doc, report) =
            convert("dbg/game0.cfg", b"GET_ITEM 1,2,3,4; GET_N_ITEM 10,5;").unwrap();
        assert_eq!(
            doc["actions"][1],
            json!({"action":"get_items_counted","items":[{"item":10,"count":5},{"item":3,"count":4}]})
        );
        assert_eq!(report.stale.len(), 2);
        let (doc, _) = convert("dbg/game0.cfg", b"GET_N_ITEM;").unwrap();
        assert_eq!(doc["actions"][0]["items"], json!([]));
    }

    #[test]
    fn map_names_keep_optional_ids() {
        let (doc, _) = convert(
            "map/map0.cfg",
            b"MAP_NAME_NUM 2; MAP_NAME \"g01\",\"Town\",\"\",1,1,2000,2; MAP_NAME \"s71\",\"Stop\",\"trn2/s36\",3,6;",
        )
        .unwrap();
        assert_eq!(doc["count"], 2);
        assert_eq!(doc["maps"][0]["add_path"], "");
        assert_eq!(doc["maps"][0]["area_no"], 2);
        assert!(doc["maps"][1].get("snd_data_id").is_none());
        assert!(convert("map/map0.cfg", b"MAP_NAME \"a\",\"b\",\"c\",1,1;").is_err());
    }

    #[test]
    fn movies_keep_optional_music() {
        let (doc, _) = convert(
            "mv.cfg",
            b"MOVIE \"Opening\",\"open.mp4\",3; MOVIE \"Ending\",\"end.mp4\";",
        )
        .unwrap();
        assert_eq!(
            doc["movies"],
            json!([{"title":"Opening","file":"open.mp4","bgm":3},{"title":"Ending","file":"end.mp4"}])
        );
    }
}
