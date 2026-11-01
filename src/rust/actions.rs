//! Names the game's motion actions in English and samples retail motion keys at action boundaries.
use super::{CoreError, CoreResult};
use serde_json::{Value, json};
use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::OnceLock;

fn fail(s: impl Into<String>) -> CoreError {
    CoreError(s.into())
}

/// English action names keyed by the game's original motion names.
pub(crate) fn motion_names() -> &'static BTreeMap<String, String> {
    static NAMES: OnceLock<BTreeMap<String, String>> = OnceLock::new();
    NAMES.get_or_init(|| {
        serde_json::from_str(include_str!("../../assets/motion_names.json"))
            .expect("motion name table is valid JSON")
    })
}

/// Translates one original motion name.
///
/// A name the table lacks names itself when it is already written in ASCII, as modded or
/// hand-written configs name new motions. Any other name the table lacks fails, so every
/// Japanese name on the disc keeps a reviewed translation.
pub(crate) fn english(game: &str) -> CoreResult<Cow<'static, str>> {
    if let Some(name) = motion_names().get(game) {
        return Ok(Cow::Borrowed(name.as_str()));
    }
    let name = snake_case(game);
    if !game.is_ascii() || name.is_empty() {
        return Err(fail(format!(
            "motion name has no English translation: {game}"
        )));
    }
    Ok(Cow::Owned(name))
}

/// Lowercases ASCII alphanumerics and joins the runs between them with single underscores.
fn snake_case(text: &str) -> String {
    let mut name = String::new();
    for c in text.chars() {
        if c.is_ascii_alphanumeric() {
            name.push(c.to_ascii_lowercase());
        } else if !name.ends_with('_') && !name.is_empty() {
            name.push('_');
        }
    }
    name.trim_end_matches('_').to_owned()
}

/// Builds the English fallback action name for a motion without KEY intervals.
pub(crate) fn stem_name(stem: &str) -> String {
    let name = snake_case(stem);
    if name.is_empty() {
        "motion".into()
    } else {
        name
    }
}

/// One game action: a CFG KEY interval of a motion, or a whole motion without KEY intervals.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct KeyAction {
    /// Original typed string argument naming the action, or null for a whole motion.
    pub game: Value,
    /// First source frame of the interval.
    pub start: u64,
    /// Last source frame of the interval.
    pub end: u64,
}

/// The ordered KEY intervals of every motion a character's profiles select.
#[derive(Clone, Default)]
pub(crate) struct ProfileActions {
    /// Actions by motion resource alias, in first KEY order.
    pub motions: BTreeMap<String, Vec<KeyAction>>,
    /// Actions of MOTION commands that name no motion resource, by profile alias and command index.
    pub unbound: BTreeMap<(String, usize), Vec<KeyAction>>,
    /// Every KEY in profile order with the identity of its motion.
    pub order: Vec<(String, KeyAction)>,
}

/// The original name of a KEY from its name argument: a typed string, or null for a KEY a game
/// leaves unnamed (`{"type":"none"}`), which takes its motion's fallback name.
pub(crate) fn key_name(argument: &Value) -> Value {
    if argument["type"] == "string" {
        argument.clone()
    } else {
        Value::Null
    }
}

/// Identifies the actions of a MOTION command that names no motion resource.
pub(crate) fn unbound_motion(profile: &str, index: usize) -> String {
    format!("{profile}#{index}")
}

fn alias(container: &str, value: &str) -> String {
    format!("{container}/{}", value.replace('\\', "/"))
}

/// Collects KEY intervals by the motion resource of their enclosing MOTION command.
pub(crate) fn key_actions(profiles: &[(String, Value)]) -> CoreResult<ProfileActions> {
    let mut actions = ProfileActions::default();
    for (profile, document) in profiles {
        let container = profile.rsplit_once('/').map_or("", |p| p.0);
        let mut current: Option<Result<String, (String, usize)>> = None;
        for (index, statement) in document["commands"]
            .as_array()
            .ok_or_else(|| fail("character profile commands missing"))?
            .iter()
            .enumerate()
        {
            let args = statement["args"].as_array().cloned().unwrap_or_default();
            match statement["command"].as_str().unwrap_or("") {
                "MOTION" => {
                    current = Some(match args.get(1).and_then(|a| a["value"].as_str()) {
                        Some(value) if !value.is_empty() => Ok(alias(container, value)),
                        _ => Err((profile.clone(), index)),
                    });
                }
                "KEY" => {
                    let Some(owner) = &current else {
                        return Err(fail(format!("KEY outside MOTION in {profile}")));
                    };
                    let (Some(game), Some(start), Some(end)) = (
                        args.first()
                            .filter(|a| a["type"] == "string" || a["type"] == "none"),
                        args.get(1).and_then(|a| a["value"].as_i64()),
                        args.get(2).and_then(|a| a["value"].as_i64()),
                    ) else {
                        return Err(fail(format!("malformed KEY in {profile}")));
                    };
                    if start < 0 {
                        return Err(fail(format!("invalid KEY interval in {profile}")));
                    }
                    // A KEY ending before it starts plays as a still pose at its start.
                    let action = KeyAction {
                        game: key_name(game),
                        start: start as u64,
                        end: end.max(start) as u64,
                    };
                    let list = match owner {
                        Ok(motion) => {
                            actions.order.push((motion.clone(), action.clone()));
                            actions.motions.entry(motion.clone()).or_default()
                        }
                        Err(key) => {
                            actions
                                .order
                                .push((unbound_motion(&key.0, key.1), action.clone()));
                            actions.unbound.entry(key.clone()).or_default()
                        }
                    };
                    if !list.contains(&action) {
                        list.push(action);
                    }
                }
                _ => {}
            }
        }
    }
    Ok(actions)
}

/// Allocates file-unique English names and records the original name each one stands for.
#[derive(Default)]
pub(crate) struct NameBook {
    /// Original typed string argument of every allocated English name.
    pub names: BTreeMap<String, Value>,
    actions: BTreeMap<(String, String, u64, u64), String>,
}

impl NameBook {
    fn allocate(&mut self, base: &str, game: &Value, reuse: bool) -> String {
        if let Some((name, _)) = self.names.iter().find(|(_, value)| reuse && *value == game) {
            return name.clone();
        }
        let mut name = base.to_owned();
        let mut suffix = 2;
        while self.names.contains_key(&name) {
            name = format!("{base}_v{suffix}");
            suffix += 1;
        }
        self.names.insert(name.clone(), game.clone());
        name
    }

    /// Names one action of a motion, giving different actions different names.
    pub fn action(
        &mut self,
        motion: &str,
        action: &KeyAction,
        fallback: &str,
    ) -> CoreResult<String> {
        let text = action.game["value"].as_str().unwrap_or("").to_owned();
        let key = (motion.to_owned(), text.clone(), action.start, action.end);
        if let Some(name) = self.actions.get(&key) {
            return Ok(name.clone());
        }
        let name = if action.game.is_null() {
            let base = stem_name(fallback);
            let mut name = base.clone();
            let mut suffix = 2;
            while self.names.contains_key(&name) {
                name = format!("{base}_v{suffix}");
                suffix += 1;
            }
            self.names.insert(name.clone(), Value::Null);
            name
        } else {
            self.allocate(&english(&text)?, &action.game, false)
        };
        self.actions.insert(key, name.clone());
        Ok(name)
    }

    /// Names a reference to an original motion or sequence name, reusing an action's name.
    pub fn reference(&mut self, game: &Value) -> CoreResult<String> {
        let text = game["value"]
            .as_str()
            .ok_or_else(|| fail("motion reference is not a string"))?;
        Ok(self.allocate(&english(text)?, game, true))
    }
}

/// The game's quaternion interpolation, including its near-opposite snap and small-angle blend.
pub(crate) fn retail_slerp(first: [f32; 4], mut second: [f32; 4], t: f32) -> [f32; 4] {
    let mut cosine: f32 = (0..4).map(|i| first[i] * second[i]).sum();
    if cosine < 0. {
        cosine = -cosine;
        for v in &mut second {
            *v = -*v;
        }
    }
    if cosine < 0.01 {
        return second;
    }
    let (mut from, mut to) = (1. - t, t);
    if 1. - cosine > 0.01 {
        let angle = cosine.clamp(-1., 1.).acos();
        let inverse = 1. / angle.sin();
        from = inverse * ((1. - t) * angle).sin();
        to = inverse * (t * angle).sin();
    }
    std::array::from_fn(|i| from * first[i] + to * second[i])
}

/// Converts a retail scalar-first rotation key into a normalized glTF rotation.
pub(crate) fn standard_rotation(value: &Value) -> [f32; 4] {
    let v: Vec<f32> = (0..4)
        .map(|i| value[i].as_f64().unwrap_or(0.) as f32)
        .collect();
    let q = [-v[1], -v[2], -v[3], v[0]];
    let norm = q.iter().map(|v| v * v).sum::<f32>().sqrt();
    if norm > 1e-20 {
        q.map(|v| v / norm)
    } else {
        [0., 0., 0., 1.]
    }
}

/// Samples one transform key list at a source frame exactly as the native evaluator does.
pub(crate) fn sample_transform(keys: &[Value], rotation: bool, frame: f32) -> Vec<f32> {
    let value = |key: &Value| -> Vec<f32> {
        if rotation {
            standard_rotation(&key["value"]).to_vec()
        } else {
            (0..3)
                .map(|i| key["value"][i].as_f64().unwrap_or(0.) as f32)
                .collect()
        }
    };
    let upper = keys.partition_point(|k| k["frame"].as_u64().unwrap_or(0) as f32 <= frame);
    if upper == 0 {
        return value(&keys[0]);
    }
    if upper == keys.len() {
        return value(&keys[upper - 1]);
    }
    let (a, b) = (&keys[upper - 1], &keys[upper]);
    let (start, end) = (
        a["frame"].as_u64().unwrap_or(0) as f32,
        b["frame"].as_u64().unwrap_or(0) as f32,
    );
    let t = (frame - start) / (end - start);
    let (x, y) = (value(a), value(b));
    if rotation {
        if t <= 0.001 {
            return x;
        }
        if t >= 0.999 {
            return y;
        }
        let q = retail_slerp([x[0], x[1], x[2], x[3]], [y[0], y[1], y[2], y[3]], t);
        let norm = q.iter().map(|v| v * v).sum::<f32>().sqrt();
        return q.map(|v| v / norm).to_vec();
    }
    (0..3).map(|i| x[i] * (1. - t) + y[i] * t).collect()
}

/// Selects the keys of one action interval, adding retail-sampled boundary keys where none exist.
///
/// Returns frames relative to the interval start and their values.
pub(crate) fn slice_keys(
    keys: &[Value],
    rotation: bool,
    start: u64,
    end: u64,
    synthesized: &mut usize,
) -> Vec<(u64, Vec<f32>)> {
    let mut out = Vec::new();
    let frames: BTreeSet<u64> = keys.iter().filter_map(|k| k["frame"].as_u64()).collect();
    if !frames.contains(&start) {
        *synthesized += 1;
        out.push((0, sample_transform(keys, rotation, start as f32)));
    }
    for key in keys {
        let frame = key["frame"].as_u64().unwrap_or(0);
        if (start..=end).contains(&frame) {
            out.push((
                frame - start,
                sample_transform(keys, rotation, frame as f32),
            ));
        }
    }
    if end != start && !frames.contains(&end) {
        *synthesized += 1;
        out.push((end - start, sample_transform(keys, rotation, end as f32)));
    }
    out
}

/// Samples one auxiliary key list at a source frame as the retail evaluator reads it.
fn sample_aux(keys: &[Value], kind: i64, frame: u64) -> Value {
    let upper = keys.partition_point(|k| k["frame"].as_u64().unwrap_or(0) <= frame);
    let value = |key: &Value| -> Vec<f64> {
        (0..4)
            .map(|i| key["value"][i].as_f64().unwrap_or(0.))
            .collect()
    };
    let result = if upper == 0 {
        value(&keys[0])
    } else if upper == keys.len() || matches!(kind, 50 | 51) {
        value(&keys[upper - 1])
    } else {
        let (a, b) = (&keys[upper - 1], &keys[upper]);
        let (start, end) = (
            a["frame"].as_u64().unwrap_or(0) as f64,
            b["frame"].as_u64().unwrap_or(0) as f64,
        );
        let t = (frame as f64 - start) / (end - start);
        let (x, y) = (value(a), value(b));
        (0..4)
            .map(|i| (x[i] * (1. - t) + y[i] * t) as f32 as f64)
            .collect()
    };
    json!({"frame":0,"value":result.iter().map(|v| *v as f32).collect::<Vec<_>>()})
}

/// Slices one auxiliary channel to an action interval, with local frames and sampled boundary keys.
pub(crate) fn slice_aux(channel: &Value, start: u64, end: u64) -> Value {
    let keys = channel["keys"].as_array().cloned().unwrap_or_default();
    let mut sliced = channel.clone();
    if keys.is_empty() {
        return sliced;
    }
    let kind = channel["type"].as_i64().unwrap_or(0);
    let frames: BTreeSet<u64> = keys.iter().filter_map(|k| k["frame"].as_u64()).collect();
    let mut out = Vec::new();
    if !frames.contains(&start) {
        out.push(sample_aux(&keys, kind, start));
    }
    for key in &keys {
        let frame = key["frame"].as_u64().unwrap_or(0);
        if (start..=end).contains(&frame) {
            let mut key = key.clone();
            key["frame"] = json!(frame - start);
            out.push(key);
        }
    }
    if end != start && !frames.contains(&end) {
        let mut key = sample_aux(&keys, kind, end);
        key["frame"] = json!(end - start);
        out.push(key);
    }
    sliced["keys"] = json!(out);
    sliced
}

/// Encodes an absolute retail sound frame as the ratio form of its named action.
pub(crate) fn ratio(frame: f64, start: u64, end: u64) -> Value {
    let span = end.saturating_sub(start) as f64;
    let value = if span == 0. {
        0.
    } else {
        ((frame - start as f64) / span).clamp(0., 1.)
    };
    json!({"type":"float","value":value})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_name_translates_to_snake_case() {
        for (game, name) in motion_names() {
            assert!(!game.is_empty());
            assert!(
                !name.is_empty()
                    && name.split('_').all(|part| !part.is_empty()
                        && part
                            .chars()
                            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit())),
                "{game} -> {name}"
            );
        }
        assert_eq!(english("立ち").unwrap(), "idle");
        assert_eq!(english("攻撃１").unwrap(), "attack_1");
        assert!(english("未知の動き").is_err());
    }

    #[test]
    fn ascii_names_the_table_lacks_name_themselves() {
        assert_eq!(english("stagger and fall").unwrap(), "stagger_and_fall");
        assert_eq!(english("64 cat stand").unwrap(), "64_cat_stand");
        assert_eq!(english("door(pat)").unwrap(), "door_pat");
        assert_eq!(english("idle").unwrap(), "idle");
        // The table still translates the names it has, ASCII ones included.
        assert_eq!(english("立ち").unwrap(), "idle");
        assert_eq!(english("3ren").unwrap(), "triple_combo");
        assert!(english("--").is_err());
        assert!(english("").is_err());
    }

    #[test]
    fn names_are_unique_per_file_and_references_reuse_actions() {
        let mut book = NameBook::default();
        let idle = json!({"type":"string","value":"立ち"});
        let first = KeyAction {
            game: idle.clone(),
            start: 5,
            end: 7,
        };
        let second = KeyAction {
            game: idle.clone(),
            start: 9,
            end: 12,
        };
        assert_eq!(book.action("a.mot", &first, "a").unwrap(), "idle");
        assert_eq!(book.action("a.mot", &first, "a").unwrap(), "idle");
        assert_eq!(book.action("b.mot", &second, "b").unwrap(), "idle_v2");
        assert_eq!(book.reference(&idle).unwrap(), "idle");
        let walk = json!({"type":"string","value":"歩き２"});
        assert_eq!(book.reference(&walk).unwrap(), "walk_2");
        let walk_ascii = json!({"type":"string","value":"歩き2"});
        assert_eq!(book.reference(&walk_ascii).unwrap(), "walk_2_v2");
        assert_eq!(book.names["walk_2_v2"], walk_ascii);
        let whole = KeyAction {
            game: Value::Null,
            start: 0,
            end: 30,
        };
        assert_eq!(book.action("e213a.mot", &whole, "e213a").unwrap(), "e213a");
    }

    #[test]
    fn boundary_keys_reproduce_retail_sampling() {
        let keys = vec![
            json!({"frame":0,"value":[1.,0.,0.,0.]}),
            json!({"frame":10,"value":[0.70710677,0.,0.70710677,0.]}),
            json!({"frame":20,"value":[0.,0.,1.,0.]}),
        ];
        let mut synthesized = 0;
        let sliced = slice_keys(&keys, true, 4, 10, &mut synthesized);
        assert_eq!(synthesized, 1);
        assert_eq!(sliced[0].0, 0);
        assert_eq!(sliced.last().unwrap().0, 6);
        let a = standard_rotation(&keys[0]["value"]);
        let b = standard_rotation(&keys[1]["value"]);
        let expected = retail_slerp(a, b, 0.4);
        let norm = expected.iter().map(|v| v * v).sum::<f32>().sqrt();
        for (actual, expected) in sliced[0].1.iter().zip(expected) {
            assert!((actual - expected / norm).abs() < 1e-6);
        }
        let translation = vec![
            json!({"frame":0,"value":[0.,0.,0.,1.]}),
            json!({"frame":10,"value":[10.,20.,30.,1.]}),
        ];
        let sliced = slice_keys(&translation, false, 2, 3, &mut synthesized);
        assert_eq!(sliced, vec![(0, vec![2., 4., 6.]), (1, vec![3., 6., 9.])]);
        let constant = slice_keys(&translation, false, 15, 18, &mut synthesized);
        assert_eq!(
            constant,
            vec![(0, vec![10., 20., 30.]), (3, vec![10., 20., 30.])]
        );
        let single = slice_keys(&translation, false, 10, 10, &mut synthesized);
        assert_eq!(single, vec![(0, vec![10., 20., 30.])]);
    }

    #[test]
    fn auxiliary_channels_slice_with_linear_and_step_boundaries() {
        let alpha = json!({"frame":3,"target":0,"type":40,"keys":[
            {"frame":0,"value":[0.,0.,0.,0.]},{"frame":10,"value":[1.,0.,0.,0.]}]});
        let sliced = slice_aux(&alpha, 5, 10);
        assert_eq!(sliced["keys"][0]["frame"], 0);
        assert_eq!(sliced["keys"][0]["value"][0], 0.5);
        assert_eq!(sliced["keys"][1]["frame"], 5);
        let visible = json!({"frame":3,"target":0,"type":50,"keys":[
            {"frame":0,"value":[0.,0.,0.,0.]},{"frame":10,"value":[1.,0.,0.,0.]}]});
        let sliced = slice_aux(&visible, 5, 12);
        assert_eq!(sliced["keys"][0]["value"][0], 0.);
        assert_eq!(sliced["keys"][1]["frame"], 5);
        assert_eq!(sliced["keys"][2]["frame"], 7);
        assert_eq!(sliced["keys"][2]["value"][0], 1.);
    }

    #[test]
    fn absolute_sound_frames_become_clamped_ratios() {
        assert_eq!(ratio(9., 218, 250)["value"], 0.);
        assert_eq!(ratio(234., 218, 250)["value"], 0.5);
        assert_eq!(ratio(300., 218, 250)["value"], 1.);
        assert_eq!(ratio(5., 5, 5)["value"], 0.);
    }

    #[test]
    fn keys_follow_their_motion_and_unbound_motions_are_kept() {
        let profile = json!({"commands":[
            {"command":"MOTION","args":[{"type":"int","value":0},{"type":"string","value":"a.mot"}]},
            {"command":"KEY_START","args":[]},
            {"command":"KEY","args":[{"type":"string","value":"立ち"},{"type":"int","value":5},{"type":"int","value":7},{"type":"float","value":0.02}]},
            {"command":"KEY","args":[{"type":"string","value":"歩き"},{"type":"int","value":11},{"type":"int","value":31},{"type":"float","value":0.5}]},
            {"command":"MOTION","args":[{"type":"int","value":1},{"type":"string","value":""}]},
            {"command":"KEY","args":[{"type":"string","value":"走り"},{"type":"int","value":0},{"type":"int","value":10},{"type":"float","value":1.0}]}
        ]});
        let actions = key_actions(&[("hero.chr/info.cfg".into(), profile)]).unwrap();
        assert_eq!(actions.motions["hero.chr/a.mot"].len(), 2);
        assert_eq!(actions.motions["hero.chr/a.mot"][1].start, 11);
        assert_eq!(actions.unbound.len(), 1);
    }
}
