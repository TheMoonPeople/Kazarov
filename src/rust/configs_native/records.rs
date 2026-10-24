//! Small record tables: shop goods, quests, the world map, scoops, fish prizes, fishing places,
//! race fish and georama analysis.
use super::*;

// --- shop_lists: CShop::AnalyzeShopList (menushop.cpp) -------------------------------------------

const SHOP: Pass = Pass {
    name: "menu_shop_tag",
    commands: &[
        Command {
            name: "SHOP",
            layout: "i *i",
        },
        Command {
            name: "PRICE",
            layout: "i i i",
        },
    ],
};

fn build_shops(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut shops = Vec::new();
    let mut prices = Vec::new();
    for row in &passes[0] {
        if row.name == "SHOP" {
            let items = (1..row.count())
                .map(|slot| row.int(slot).map(|v| json!(v)))
                .collect::<CoreResult<Vec<_>>>()?;
            shops.push(json!({"id": row.int(0)?, "items": items}));
        } else {
            prices.push(json!({"item": row.int(0)?, "buy": row.int(1)?, "sell": row.int(2)?}));
        }
    }
    Ok(json!({"shops": shops, "prices": prices}))
}

fn flatten_shops(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for shop in array(document, "shops")? {
        let mut vals = vec![Some(int_field(shop, "id")?)];
        for item in array(shop, "items")? {
            vals.push(Some(int_val(item)?));
        }
        rows.push(Eff::new("SHOP", vals));
    }
    for price in array(document, "prices")? {
        rows.push(row_of(
            price,
            "PRICE",
            &[("item", 'i'), ("buy", 'i'), ("sell", 'i')],
        )?);
    }
    Ok(vec![rows])
}

const SHOP_LISTS: Family = Family {
    kind: "shop_lists",
    passes: &[&SHOP],
    ignored: &[],
    build: build_shops,
    flatten: flatten_shops,
    ordered: false,
};

/// Converts a record row to an object whose keys follow the slots, leaving out unread slots.
fn record(row: &Eff, keys: &[(&str, char)]) -> Value {
    let mut obj = Obj::new();
    for (slot, (key, _)) in keys.iter().enumerate() {
        obj = obj.opt(key, slot_json(row, slot));
    }
    obj.done()
}

// --- quests: CQuestManager::LoadCfg (quest.cpp) ------------------------------------------------

const QUEST: Pass = Pass {
    name: "quest_cmd_tag",
    commands: &[
        Command {
            name: "NUM",
            layout: "i",
        },
        Command {
            name: "NEW",
            layout: "i s",
        },
        Command {
            name: "COMENT",
            layout: "i s",
        },
        Command {
            name: "END",
            layout: "",
        },
    ],
};

fn build_quests(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = cursor.expect("NUM")?.int(0)?;
    let mut quests = Vec::new();
    while let Some(new) = cursor.take("NEW") {
        let mut texts = Vec::new();
        while let Some(comment) = cursor.take("COMENT") {
            // COMENT 0 is the description and COMENT n reaction n - 1; the scripts give them in order.
            if comment.int(0)? != texts.len() as i32 {
                return Err(fail("quest comments are not numbered in order"));
            }
            texts.push(text_json(&comment.text(1)?));
        }
        cursor.expect("END")?;
        let mut quest = Obj::new()
            .put("id", json!(new.int(0)?))
            .put("name", text_json(&new.text(1)?));
        if !texts.is_empty() {
            let comment = texts.remove(0);
            quest = quest
                .put("comment", comment)
                .put("reactions", Value::Array(texts));
        }
        quests.push(quest.done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "quests": quests}))
}

fn flatten_quests(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new("NUM", vec![Some(int_field(document, "count")?)])];
    for quest in array(document, "quests")? {
        rows.push(Eff::new(
            "NEW",
            vec![
                Some(int_field(quest, "id")?),
                Some(text_field(quest, "name")?),
            ],
        ));
        if let Some(comment) = quest.get("comment") {
            rows.push(Eff::new(
                "COMENT",
                vec![Some(Val::I(0)), Some(text_val(comment)?)],
            ));
            for (index, reaction) in array(quest, "reactions")?.iter().enumerate() {
                rows.push(Eff::new(
                    "COMENT",
                    vec![Some(Val::I(index as i32 + 1)), Some(text_val(reaction)?)],
                ));
            }
        }
        rows.push(Eff::new("END", vec![]));
    }
    Ok(vec![rows])
}

const QUESTS: Family = Family {
    kind: "quests",
    passes: &[&QUEST],
    ignored: &[],
    build: build_quests,
    flatten: flatten_quests,
    ordered: true,
};

// --- world_map: worldmap_analyze (menumap.cpp) --------------------------------------------------

const WMAP: Pass = Pass {
    name: "menu_wmap_analyze_tag",
    commands: &[
        Command {
            name: "POS_NUM",
            layout: "i",
        },
        Command {
            name: "POS",
            layout: "i i i i i i i i",
        },
        Command {
            name: "AREA_NUM",
            layout: "i",
        },
        Command {
            name: "AREA",
            layout: "i i i i i i i s",
        },
    ],
};

const POS_KEYS: &[(&str, char)] = &[
    ("index", 'i'),
    ("map", 'i'),
    ("loop", 'i'),
    ("area", 'i'),
    ("dungeon", 'i'),
    ("floor", 'i'),
    ("flag", 'i'),
    ("type", 'i'),
];
const AREA_KEYS: &[(&str, char)] = &[
    ("area", 'i'),
    ("map", 'i'),
    ("x", 'i'),
    ("y", 'i'),
    ("name_x", 'i'),
    ("name_y", 'i'),
    ("name_side", 'i'),
    ("title", 's'),
];

fn build_world_map(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let positions_capacity = cursor.expect("POS_NUM")?.int(0)?;
    let mut positions = Vec::new();
    while let Some(row) = cursor.take("POS") {
        positions.push(record(row, POS_KEYS));
    }
    let areas_capacity = cursor.expect("AREA_NUM")?.int(0)?;
    let mut areas = Vec::new();
    while let Some(row) = cursor.take("AREA") {
        areas.push(record(row, AREA_KEYS));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(
        json!({"positions_capacity": positions_capacity, "positions": positions,
              "areas_capacity": areas_capacity, "areas": areas}),
    )
}

fn flatten_world_map(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new(
        "POS_NUM",
        vec![Some(int_field(document, "positions_capacity")?)],
    )];
    for pos in array(document, "positions")? {
        rows.push(row_of(pos, "POS", POS_KEYS)?);
    }
    rows.push(Eff::new(
        "AREA_NUM",
        vec![Some(int_field(document, "areas_capacity")?)],
    ));
    for area in array(document, "areas")? {
        rows.push(row_of(area, "AREA", AREA_KEYS)?);
    }
    Ok(vec![rows])
}

const WORLD_MAP: Family = Family {
    kind: "world_map",
    passes: &[&WMAP],
    ignored: &[],
    build: build_world_map,
    flatten: flatten_world_map,
    ordered: true,
};

// --- scoop_texts: AnalyzeScoopString (inventmn.cpp) ----------------------------------------------

const SCOOP: Pass = Pass {
    name: "menu_scoop_str_tag",
    commands: &[Command {
        name: "STR",
        layout: "i s",
    }],
};

const SCOOP_KEYS: &[(&str, char)] = &[("id", 'i'), ("text", 's')];

fn build_scoops(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let entries: Vec<Value> = passes[0]
        .iter()
        .map(|row| record(row, SCOOP_KEYS))
        .collect();
    Ok(json!({ "entries": entries }))
}

fn flatten_scoops(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "entries")?
        .iter()
        .map(|e| row_of(e, "STR", SCOOP_KEYS))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(vec![rows])
}

const SCOOP_TEXTS: Family = Family {
    kind: "scoop_texts",
    passes: &[&SCOOP],
    ignored: &[],
    build: build_scoops,
    flatten: flatten_scoops,
    ordered: true,
};

// --- fish_prizes: LoadFishPrize (menuaqua.cpp) ---------------------------------------------------

const PRIZE: Pass = Pass {
    name: "gyoprize_tag",
    commands: &[
        Command {
            name: "NUM",
            layout: "i",
        },
        // _PRIZE_GROUP always reads the prize count and eight rates.
        Command {
            name: "GRP",
            layout: "i i i i i i i i i",
        },
        Command {
            name: "PRIZE",
            layout: "i i i i i i i",
        },
    ],
};

fn build_prizes(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = cursor.expect("NUM")?.int(0)?;
    let mut groups = Vec::new();
    while let Some(group) = cursor.take("GRP") {
        let mut prizes = Vec::new();
        while let Some(prize) = cursor.take("PRIZE") {
            let ranks = (0..3)
                .map(|rank| Ok(json!([prize.int(1 + rank * 2)?, prize.int(2 + rank * 2)?])))
                .collect::<CoreResult<Vec<_>>>()?;
            prizes.push(json!({"rate": prize.int(0)?, "ranks": ranks}));
        }
        groups.push(json!({"prize_count": group.int(0)?, "rates": ints_json(group, 1, 8)?, "prizes": prizes}));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "groups": groups}))
}

fn flatten_prizes(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new("NUM", vec![Some(int_field(document, "count")?)])];
    for group in array(document, "groups")? {
        let mut vals = vec![Some(int_field(group, "prize_count")?)];
        vals.extend(int_slots(group, "rates", 8)?);
        rows.push(Eff::new("GRP", vals));
        for prize in array(group, "prizes")? {
            let mut vals = vec![Some(int_field(prize, "rate")?)];
            let ranks = array(prize, "ranks")?;
            if ranks.len() != 3 {
                return Err(fail("a prize has three ranks"));
            }
            for rank in ranks {
                let pair = rank
                    .as_array()
                    .filter(|p| p.len() == 2)
                    .ok_or_else(|| fail("a rank is [item, num]"))?;
                vals.push(Some(int_val(&pair[0])?));
                vals.push(Some(int_val(&pair[1])?));
            }
            rows.push(Eff::new("PRIZE", vals));
        }
    }
    Ok(vec![rows])
}

const FISH_PRIZES: Family = Family {
    kind: "fish_prizes",
    passes: &[&PRIZE],
    ignored: &[],
    build: build_prizes,
    flatten: flatten_prizes,
    ordered: true,
};

// --- fishing_places: LoadFishPlaceData (fishing.cpp) ---------------------------------------------

const FISHING: Pass = Pass {
    name: "fishing_tag",
    commands: &[
        Command {
            name: "FISH_MAP_NUM",
            layout: "i",
        },
        Command {
            name: "FISH_MAP",
            layout: "i i?",
        },
        Command {
            name: "FISH_PLACE",
            layout: "i s f f f f f",
        },
        Command {
            name: "FISH",
            layout: "i f f",
        },
        Command {
            name: "FISH_MAP_END",
            layout: "",
        },
    ],
};

const FISH_KEYS: &[(&str, char)] = &[("fish", 'i'), ("rate", 'f'), ("wait_bias", 'f')];

fn build_fishing(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let count = cursor.expect("FISH_MAP_NUM")?.int(0)?;
    let mut maps = Vec::new();
    while let Some(map) = cursor.take("FISH_MAP") {
        let mut obj = Obj::new()
            .put("map", json!(map.int(0)?))
            .opt("exclusive", slot_json(map, 1));
        if let Some(place) = cursor.take("FISH_PLACE") {
            obj = obj.put(
                "place",
                json!({"area_type": place.int(0)?, "name": text_json(&place.text(1)?),
                       "params": floats_json(place, 2, 5)?}),
            );
        }
        let mut fish = Vec::new();
        while let Some(row) = cursor.take("FISH") {
            fish.push(record(row, FISH_KEYS));
        }
        cursor.expect("FISH_MAP_END")?;
        maps.push(obj.put("fish", Value::Array(fish)).done());
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({"count": count, "maps": maps}))
}

fn flatten_fishing(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = vec![Eff::new(
        "FISH_MAP_NUM",
        vec![Some(int_field(document, "count")?)],
    )];
    for map in array(document, "maps")? {
        rows.push(Eff::new(
            "FISH_MAP",
            vec![
                Some(int_field(map, "map")?),
                opt_field(map, "exclusive", int_val)?,
            ],
        ));
        if let Some(place) = map.get("place") {
            let mut vals = vec![
                Some(int_field(place, "area_type")?),
                Some(text_field(place, "name")?),
            ];
            vals.extend(float_slots(place, "params", 5)?);
            rows.push(Eff::new("FISH_PLACE", vals));
        }
        for fish in array(map, "fish")? {
            rows.push(row_of(fish, "FISH", FISH_KEYS)?);
        }
        rows.push(Eff::new("FISH_MAP_END", vec![]));
    }
    Ok(vec![rows])
}

const FISHING_PLACES: Family = Family {
    kind: "fishing_places",
    passes: &[&FISHING],
    ignored: &[],
    build: build_fishing,
    flatten: flatten_fishing,
    ordered: true,
};

// --- race_fish: CGyoraceFishData::LoadData (menuaqua.cpp) ----------------------------------------

const GYORACE: Pass = Pass {
    name: "gyorace_tag",
    commands: &[
        Command {
            name: "NUM",
            layout: "i i",
        },
        Command {
            name: "DAT",
            layout: "i s i i i i i i i",
        },
    ],
};

const RACE_FISH_KEYS: &[(&str, char)] = &[
    ("id", 'i'),
    ("name", 's'),
    ("kind", 'i'),
    ("battle", 'i'),
    ("boost", 'i'),
    ("endurance", 'i'),
    ("tenacity", 'i'),
    ("stamina", 'i'),
    ("size", 'i'),
];

fn build_race_fish(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let mut classes = Vec::new();
    while let Some(num) = cursor.take("NUM") {
        let mut fish = Vec::new();
        while let Some(row) = cursor.take("DAT") {
            fish.push(record(row, RACE_FISH_KEYS));
        }
        classes.push(json!({"class": num.int(0)?, "count": num.int(1)?, "fish": fish}));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({ "classes": classes }))
}

fn flatten_race_fish(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for class in array(document, "classes")? {
        rows.push(Eff::new(
            "NUM",
            vec![
                Some(int_field(class, "class")?),
                Some(int_field(class, "count")?),
            ],
        ));
        for fish in array(class, "fish")? {
            rows.push(row_of(fish, "DAT", RACE_FISH_KEYS)?);
        }
    }
    Ok(vec![rows])
}

const RACE_FISH: Family = Family {
    kind: "race_fish",
    passes: &[&GYORACE],
    ignored: &[],
    build: build_race_fish,
    flatten: flatten_race_fish,
    ordered: true,
};

// --- debug_race_fish: LoadGyorace (editdebug.cpp) ------------------------------------------------

const GYOFISH: Pass = Pass {
    name: "gyofish_tags",
    commands: &[Command {
        name: "GYOFISH",
        layout: "s i i i i i i i i i",
    }],
};

const GYOFISH_KEYS: &[(&str, char)] = &[
    ("name", 's'),
    ("item", 'i'),
    ("color", 'i'),
    ("kind", 'i'),
    ("tactics", 'i'),
    ("battle", 'i'),
    ("stamina", 'i'),
    ("boost", 'i'),
    ("endurance", 'i'),
    ("tenacity", 'i'),
];

fn build_debug_race_fish(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let fish: Vec<Value> = passes[0]
        .iter()
        .map(|row| record(row, GYOFISH_KEYS))
        .collect();
    Ok(json!({ "fish": fish }))
}

fn flatten_debug_race_fish(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let rows = array(document, "fish")?
        .iter()
        .map(|fish| row_of(fish, "GYOFISH", GYOFISH_KEYS))
        .collect::<CoreResult<Vec<_>>>()?;
    Ok(vec![rows])
}

const DEBUG_RACE_FISH: Family = Family {
    kind: "debug_race_fish",
    passes: &[&GYOFISH],
    ignored: &[],
    build: build_debug_race_fish,
    flatten: flatten_debug_race_fish,
    ordered: true,
};

// --- georama_analysis: LoadEditAnalyzeData (editdata.cpp) ----------------------------------------

const ANALYSIS: Pass = Pass {
    name: "editdata_tag",
    commands: &[
        Command {
            name: "GEO_ANALYZE",
            layout: "i",
        },
        Command {
            name: "CONDITION",
            layout: "i s i?",
        },
        Command {
            name: "ANALYZE",
            layout: "i i s i?",
        },
        Command {
            name: "CON_NO",
            layout: "*i",
        },
        Command {
            name: "ON_PARTS",
            layout: "s",
        },
        Command {
            name: "OFF_PARTS",
            layout: "s",
        },
        Command {
            name: "PERCENT",
            layout: "i",
        },
        Command {
            name: "END_ANALYZE",
            layout: "",
        },
        Command {
            name: "END_GEO_ANALYZE",
            layout: "",
        },
    ],
};

const CONDITION_KEYS: &[(&str, char)] = &[("index", 'i'), ("name", 's'), ("geo_floor", 'i')];
const ENTRY_KEYS: &[(&str, char)] = &[
    ("index", 'i'),
    ("unk_10", 'i'),
    ("message", 's'),
    ("geo_floor", 'i'),
];

fn build_analysis(_: &str, passes: &[Vec<Eff>]) -> CoreResult<Value> {
    let mut cursor = Cursor::new(&passes[0]);
    let mut sources = Vec::new();
    while let Some(source) = cursor.take("GEO_ANALYZE") {
        let mut conditions = Vec::new();
        while let Some(row) = cursor.take("CONDITION") {
            conditions.push(record(row, CONDITION_KEYS));
        }
        let mut entries = Vec::new();
        while let Some(row) = cursor.take("ANALYZE") {
            let Value::Object(map) = record(row, ENTRY_KEYS) else {
                unreachable!()
            };
            let mut entry = Obj(map);
            if let Some(numbers) = cursor.take("CON_NO") {
                entry = entry.put("condition_numbers", ints_json(numbers, 0, numbers.count())?);
            }
            if let Some(on) = cursor.take("ON_PARTS") {
                entry = entry.put("on_parts", text_json(&on.text(0)?));
            }
            if let Some(off) = cursor.take("OFF_PARTS") {
                entry = entry.put("off_parts", text_json(&off.text(0)?));
            }
            if let Some(percent) = cursor.take("PERCENT") {
                entry = entry.put("percent", json!(percent.int(0)?));
            }
            cursor.expect("END_ANALYZE")?;
            entries.push(entry.done());
        }
        cursor.expect("END_GEO_ANALYZE")?;
        sources.push(json!({"id": source.int(0)?, "conditions": conditions, "entries": entries}));
    }
    if !cursor.done() {
        return Err(cursor.unexpected());
    }
    Ok(json!({ "sources": sources }))
}

fn flatten_analysis(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let mut rows = Vec::new();
    for source in array(document, "sources")? {
        rows.push(Eff::new(
            "GEO_ANALYZE",
            vec![Some(int_field(source, "id")?)],
        ));
        for condition in array(source, "conditions")? {
            rows.push(row_of(condition, "CONDITION", CONDITION_KEYS)?);
        }
        for entry in array(source, "entries")? {
            rows.push(row_of(entry, "ANALYZE", ENTRY_KEYS)?);
            if entry.get("condition_numbers").is_some() {
                let vals = array(entry, "condition_numbers")?
                    .iter()
                    .map(|v| int_val(v).map(Some))
                    .collect::<CoreResult<Vec<_>>>()?;
                rows.push(Eff::new("CON_NO", vals));
            }
            if let Some(on) = entry.get("on_parts") {
                rows.push(Eff::new("ON_PARTS", vec![Some(text_val(on)?)]));
            }
            if let Some(off) = entry.get("off_parts") {
                rows.push(Eff::new("OFF_PARTS", vec![Some(text_val(off)?)]));
            }
            if let Some(percent) = entry.get("percent") {
                rows.push(Eff::new("PERCENT", vec![Some(int_val(percent)?)]));
            }
            rows.push(Eff::new("END_ANALYZE", vec![]));
        }
        rows.push(Eff::new("END_GEO_ANALYZE", vec![]));
    }
    Ok(vec![rows])
}

const GEORAMA_ANALYSIS: Family = Family {
    kind: "georama_analysis",
    passes: &[&ANALYSIS],
    ignored: &[],
    build: build_analysis,
    flatten: flatten_analysis,
    ordered: true,
};

pub(super) const FAMILIES: &[&Family] = &[
    &SHOP_LISTS,
    &QUESTS,
    &WORLD_MAP,
    &SCOOP_TEXTS,
    &FISH_PRIZES,
    &FISHING_PLACES,
    &RACE_FISH,
    &DEBUG_RACE_FISH,
    &GEORAMA_ANALYSIS,
];

pub(super) fn kind(path: &str, leaf: &str) -> Option<&'static str> {
    let parts: Vec<&str> = path.split('/').collect();
    let root = parts.len() == 1;
    let kind = if (path.starts_with("menu/shoplst") || path.starts_with("menu/shopusa"))
        && parts.len() == 2
        || path.ends_with("/shop.pac/shoplst.cfg")
    {
        "shop_lists"
    } else if path.contains("/quest.pac/") && leaf.starts_with("quest") {
        "quests"
    } else if parts.len() == 3
        && parts[0] == "menu"
        && matches!(leaf, "wldmap.cfg" | "wmap.cfg" | "wdmap.cfg")
    {
        "world_map"
    } else if leaf == "scoop.cfg" {
        "scoop_texts"
    } else if root && matches!(leaf, "gyop.cfg" | "uofp.cfg") {
        "fish_prizes"
    } else if path == "sg/fish/fishing.cfg" {
        "fishing_places"
    } else if path.starts_with("sg/gyo/")
        && (leaf.starts_with("rfd") || leaf.starts_with("racefish"))
    {
        "race_fish"
    } else if root && leaf == "gyorace.cfg" {
        "debug_race_fish"
    } else if root && leaf.starts_with("geo") {
        "georama_analysis"
    } else {
        return None;
    };
    Some(kind)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn convert(alias: &str, script: &[u8]) -> Value {
        let document = super::super::super::configs::parse(script).unwrap();
        super::super::build(alias, &document).unwrap().0
    }

    #[test]
    fn shops_keep_goods_and_prices() {
        let doc = convert(
            "menu/shoplst1.cfg",
            b"SHOP 1,10,11,12; SHOP 2; PRICE 10,100,50;",
        );
        assert_eq!(doc["kind"], "shop_lists");
        assert_eq!(doc["shops"][0]["items"], json!([10, 11, 12]));
        assert_eq!(doc["shops"][1]["items"], json!([]));
        assert_eq!(doc["prices"][0], json!({"item":10,"buy":100,"sell":50}));
    }
}
