//! Builds native semantic configuration documents from the commands of a tagged CFG script.
//!
//! A family describes the retail callbacks that read its script: one tag table per pass, each
//! command's argument slots, their types and the argument counts that guard them. The commands are
//! first reduced to the values those callbacks read (`effective`), simulating retail's argument stack,
//! whose slots keep the previous command's values when a callback reads past its own arguments. The
//! family then nests those values into its document (`build`), and `flatten` turns a document back
//! into the same values. Conversion fails unless `flatten(build(values))` reproduces them exactly, so
//! every published document carries all the semantics of the script it replaces.
use super::{CoreError, CoreResult};
use serde_json::{Map, Number, Value, json};
use std::collections::BTreeMap;

#[path = "configs_native/dungeon.rs"]
mod dungeon;
#[path = "configs_native/game.rs"]
mod game;
#[path = "configs_native/items.rs"]
mod items;
#[path = "configs_native/map.rs"]
mod map;
#[path = "configs_native/menu.rs"]
mod menu;
#[path = "configs_native/records.rs"]
mod records;
#[path = "configs_native/scene.rs"]
mod scene;
#[path = "configs_native/villagers.rs"]
mod villagers;

/// Schema version of every native semantic configuration document.
pub(crate) const SCHEMA_VERSION: i64 = 2;
/// Argument slots of retail's interpreter stack.
const STACK_SLOTS: usize = 64;

pub(crate) fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}

/// Game text decoded from CP932, with the original bytes when the text cannot reproduce them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Text {
    pub text: String,
    pub game_bytes: Option<String>,
}

/// One value a retail callback reads, already coerced to the type it reads it as.
#[derive(Clone, Debug)]
pub(crate) enum Val {
    I(i32),
    F(f32),
    S(Option<Text>),
}

impl PartialEq for Val {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Val::I(a), Val::I(b)) => a == b,
            (Val::F(a), Val::F(b)) => a.to_bits() == b.to_bits(),
            (Val::S(a), Val::S(b)) => a == b,
            _ => false,
        }
    }
}

/// Raw typed operand as the interpreter pushes it.
#[derive(Clone, Debug)]
enum Arg {
    Invalid,
    I(i32),
    F(f32),
    S(Text),
}

/// Type a callback reads a slot as; `Raw` keeps the operand's own type.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Ty {
    I,
    F,
    S,
    Raw,
}

/// One slot read by a callback: its type and, when guarded, the argument count it needs to exceed.
#[derive(Clone, Copy, Debug)]
struct Slot {
    ty: Ty,
    guard: Option<usize>,
}

/// Parsed argument layout of one callback.
#[derive(Clone, Debug)]
struct Layout {
    slots: Vec<Slot>,
    rest: Option<Ty>,
    noop: bool,
}

/// Argument layout of one command, written as space-separated slots.
///
/// `i`, `f`, `s` and `v` read an integer, a float, a string, or the operand as typed; a `?` suffix
/// reads the slot only when the argument count exceeds its index, `?N` only when it exceeds N; `*t`
/// reads every remaining argument as `t`. `-` marks a command whose callback reads nothing and has
/// no effect: it still pushes its operands but produces no value.
pub(crate) struct Command {
    pub name: &'static str,
    pub layout: &'static str,
}

/// Commands of one retail tag table.
pub(crate) struct Pass {
    pub name: &'static str,
    pub commands: &'static [Command],
}

/// Values one callback of a pass reads, in its declared slot order.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Eff {
    pub name: String,
    pub vals: Vec<Option<Val>>,
}

impl Eff {
    pub fn new(name: &str, vals: Vec<Option<Val>>) -> Eff {
        Eff {
            name: name.into(),
            vals,
        }
    }
    pub fn int(&self, slot: usize) -> CoreResult<i32> {
        match self.vals.get(slot) {
            Some(Some(Val::I(v))) => Ok(*v),
            _ => Err(fail(format!(
                "{} slot {slot} is not a read integer",
                self.name
            ))),
        }
    }
    pub fn float(&self, slot: usize) -> CoreResult<f32> {
        match self.vals.get(slot) {
            Some(Some(Val::F(v))) => Ok(*v),
            _ => Err(fail(format!(
                "{} slot {slot} is not a read float",
                self.name
            ))),
        }
    }
    pub fn text(&self, slot: usize) -> CoreResult<Option<Text>> {
        match self.vals.get(slot) {
            Some(Some(Val::S(v))) => Ok(v.clone()),
            _ => Err(fail(format!(
                "{} slot {slot} is not a read string",
                self.name
            ))),
        }
    }
    pub fn has(&self, slot: usize) -> bool {
        matches!(self.vals.get(slot), Some(Some(_)))
    }
    pub fn count(&self) -> usize {
        self.vals.len()
    }
}

/// Stale reads and dropped commands found while converting one script.
#[derive(Default, Debug)]
pub(crate) struct Report {
    pub stale: Vec<String>,
    pub dropped: Vec<String>,
}

fn parse_layout(layout: &str) -> CoreResult<Layout> {
    let mut out = Layout {
        slots: Vec::new(),
        rest: None,
        noop: false,
    };
    if layout == "-" {
        out.noop = true;
        return Ok(out);
    }
    for token in layout.split_whitespace() {
        if out.rest.is_some() {
            return Err(fail(format!("layout {layout}: slots after *")));
        }
        let ty_of = |c: char| match c {
            'i' => Ok(Ty::I),
            'f' => Ok(Ty::F),
            's' => Ok(Ty::S),
            'v' => Ok(Ty::Raw),
            _ => Err(fail(format!("layout {layout}: bad type {c}"))),
        };
        let mut chars = token.chars();
        let first = chars.next().unwrap();
        if first == '*' {
            out.rest = Some(ty_of(chars.next().ok_or_else(|| fail("layout: * type"))?)?);
            continue;
        }
        let ty = ty_of(first)?;
        let rest: String = chars.collect();
        let guard = if rest.is_empty() {
            None
        } else if rest == "?" {
            Some(out.slots.len())
        } else if let Some(n) = rest.strip_prefix('?') {
            Some(
                n.parse::<usize>()
                    .map_err(|_| fail(format!("layout {layout}: bad guard")))?,
            )
        } else {
            return Err(fail(format!("layout {layout}: bad token {token}")));
        };
        out.slots.push(Slot { ty, guard });
    }
    Ok(out)
}

fn text_of(arg: &Value) -> CoreResult<Text> {
    Ok(Text {
        text: arg["value"]
            .as_str()
            .ok_or_else(|| fail("string operand without text"))?
            .to_owned(),
        game_bytes: arg["game_bytes"].as_str().map(str::to_owned),
    })
}

fn decode_arg(arg: &Value) -> CoreResult<Arg> {
    Ok(match arg["type"].as_str() {
        Some("int") => Arg::I(
            arg["value"]
                .as_i64()
                .and_then(|v| i32::try_from(v).ok())
                .ok_or_else(|| fail("invalid integer operand"))?,
        ),
        Some("float") => Arg::F(
            arg["value"]
                .as_f64()
                .ok_or_else(|| fail("invalid float operand"))? as f32,
        ),
        Some("string") => Arg::S(text_of(arg)?),
        Some("invalid") => Arg::Invalid,
        _ => return Err(fail("unknown operand type")),
    })
}

fn coerce(arg: &Arg, ty: Ty) -> Val {
    match ty {
        Ty::I => Val::I(match arg {
            Arg::I(v) => *v,
            Arg::F(v) => *v as i32,
            _ => 0,
        }),
        Ty::F => Val::F(match arg {
            Arg::I(v) => *v as f32,
            Arg::F(v) => *v,
            _ => 0.0,
        }),
        Ty::S => Val::S(match arg {
            Arg::S(t) => Some(t.clone()),
            _ => None,
        }),
        Ty::Raw => match arg {
            Arg::I(v) => Val::I(*v),
            Arg::F(v) => Val::F(*v),
            Arg::S(t) => Val::S(Some(t.clone())),
            Arg::Invalid => Val::S(None),
        },
    }
}

/// A script command: its name and typed operands.
#[derive(Clone, Debug)]
pub(crate) struct Source {
    pub name: String,
    args: Vec<Arg>,
}

/// Reads a v1 command list (the tokenizer's output) into commands.
pub(crate) fn sources(document: &Value) -> CoreResult<Vec<Source>> {
    let mut out = Vec::new();
    for command in document["commands"]
        .as_array()
        .ok_or_else(|| fail("CFG commands missing"))?
    {
        let name = command["command"]
            .as_str()
            .ok_or_else(|| fail("binary CFG command without a resolved name"))?
            .to_owned();
        let args = command["args"]
            .as_array()
            .ok_or_else(|| fail("CFG arguments missing"))?
            .iter()
            .map(decode_arg)
            .collect::<CoreResult<Vec<_>>>()?;
        out.push(Source { name, args });
    }
    Ok(out)
}

/// Reduces commands to the values one pass's callbacks read.
///
/// Commands the pass's table does not name are skipped without touching the stack, as the retail
/// interpreter skips them. A callback reading past its own arguments sees the stack slot left by the
/// last command that pushed one; such reads are listed in the report and refused for strings, whose
/// storage retail reuses.
pub(crate) fn effective(
    pass: &Pass,
    commands: &[Source],
    report: &mut Report,
) -> CoreResult<Vec<Eff>> {
    let mut layouts = BTreeMap::new();
    for command in pass.commands {
        // Retail resolves a repeated name to its first table entry.
        layouts
            .entry(command.name)
            .or_insert(parse_layout(command.layout)?);
    }
    let mut stack: Vec<Arg> = vec![Arg::Invalid; STACK_SLOTS];
    let mut out = Vec::new();
    for command in commands {
        let Some(layout) = layouts.get(command.name.as_str()) else {
            continue;
        };
        let argc = command.args.len().min(STACK_SLOTS);
        for (slot, arg) in command.args.iter().take(STACK_SLOTS).enumerate() {
            stack[slot] = arg.clone();
        }
        if layout.noop {
            continue;
        }
        let mut vals = Vec::new();
        for (index, slot) in layout.slots.iter().enumerate() {
            if slot.guard.is_some_and(|guard| argc <= guard) {
                vals.push(None);
                continue;
            }
            if index >= argc {
                if index >= STACK_SLOTS {
                    return Err(fail(format!("{} reads past the stack", command.name)));
                }
                // A stale string points into storage the interpreter reuses for every command.
                if matches!(stack[index], Arg::S(_)) && (slot.ty == Ty::S || slot.ty == Ty::Raw) {
                    return Err(fail(format!(
                        "{} reads a stale string from slot {index}",
                        command.name
                    )));
                }
                report
                    .stale
                    .push(format!("{} slot {index} ({})", command.name, pass.name));
            }
            vals.push(Some(coerce(&stack[index.min(STACK_SLOTS - 1)], slot.ty)));
        }
        if let Some(ty) = layout.rest {
            for arg in stack.iter().take(argc).skip(layout.slots.len()) {
                vals.push(Some(coerce(arg, ty)));
            }
        }
        out.push(Eff::new(&command.name, vals));
    }
    Ok(out)
}

/// Rejects commands that none of a family's passes name, other than those it lists as ignored.
fn check_known(
    alias: &str,
    passes: &[&Pass],
    ignored: &[&str],
    commands: &[Source],
    report: &mut Report,
) -> CoreResult<()> {
    for command in commands {
        let known = passes
            .iter()
            .any(|pass| pass.commands.iter().any(|c| c.name == command.name));
        if known {
            continue;
        }
        if ignored.contains(&command.name.as_str()) {
            report.dropped.push(command.name.clone());
            continue;
        }
        return Err(fail(format!(
            "{alias}: command {} is read by no consumer of this configuration",
            command.name
        )));
    }
    Ok(())
}

// --- JSON value helpers shared by the families -------------------------------------------------

/// Serializes a float with the shortest decimal that reads back as the same f32.
pub(crate) fn float_json(value: f32) -> Value {
    let short = format!("{value}");
    let parsed = short.parse::<f64>().ok().filter(|v| *v as f32 == value);
    let number = Number::from_f64(parsed.unwrap_or(value as f64)).expect("finite float");
    Value::Number(number)
}

pub(crate) fn text_json(text: &Option<Text>) -> Value {
    match text {
        None => Value::Null,
        Some(Text {
            text,
            game_bytes: None,
        }) => json!(text),
        Some(Text {
            text,
            game_bytes: Some(bytes),
        }) => json!({"text": text, "game_bytes": bytes}),
    }
}

pub(crate) fn val_json(val: &Val) -> Value {
    match val {
        Val::I(v) => json!(v),
        Val::F(v) => float_json(*v),
        Val::S(t) => text_json(t),
    }
}

/// Serializes a raw-typed value so that its type survives: integers as integers, floats as
/// `{"float": x}`, strings as strings.
pub(crate) fn raw_json(val: &Val) -> Value {
    match val {
        Val::F(v) => json!({ "float": float_json(*v) }),
        other => val_json(other),
    }
}

pub(crate) fn raw_of(value: &Value) -> CoreResult<Val> {
    if let Some(float) = value.get("float") {
        return Ok(Val::F(f32_of(float)?));
    }
    if value.is_i64() || value.is_u64() {
        return Ok(Val::I(i32_of(value)?));
    }
    Ok(Val::S(text_of_json(value)?))
}

pub(crate) fn i32_of(value: &Value) -> CoreResult<i32> {
    value
        .as_i64()
        .and_then(|v| i32::try_from(v).ok())
        .ok_or_else(|| fail(format!("expected a 32-bit integer, found {value}")))
}

pub(crate) fn f32_of(value: &Value) -> CoreResult<f32> {
    let v = value
        .as_f64()
        .ok_or_else(|| fail(format!("expected a number, found {value}")))? as f32;
    if !v.is_finite() {
        return Err(fail("non-finite number"));
    }
    Ok(v)
}

pub(crate) fn text_of_json(value: &Value) -> CoreResult<Option<Text>> {
    match value {
        Value::Null => Ok(None),
        Value::String(text) => Ok(Some(Text {
            text: text.clone(),
            game_bytes: None,
        })),
        Value::Object(object) => {
            let text = object
                .get("text")
                .and_then(Value::as_str)
                .ok_or_else(|| fail("game text object without text"))?;
            let bytes = object
                .get("game_bytes")
                .and_then(Value::as_str)
                .ok_or_else(|| fail("game text object without bytes"))?;
            if !bytes.len().is_multiple_of(2) || !bytes.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err(fail("invalid game text bytes"));
            }
            Ok(Some(Text {
                text: text.into(),
                game_bytes: Some(bytes.into()),
            }))
        }
        _ => Err(fail(format!("expected game text, found {value}"))),
    }
}

/// Builds an object from key/value pairs, leaving out keys whose value is absent.
pub(crate) struct Obj(pub Map<String, Value>);

impl Obj {
    pub fn new() -> Obj {
        Obj(Map::new())
    }
    pub fn put(mut self, key: &str, value: Value) -> Obj {
        self.0.insert(key.into(), value);
        self
    }
    pub fn opt(mut self, key: &str, value: Option<Value>) -> Obj {
        if let Some(value) = value {
            self.0.insert(key.into(), value);
        }
        self
    }
    pub fn done(self) -> Value {
        Value::Object(self.0)
    }
}

/// Converts one effective slot to JSON by its read type.
pub(crate) fn slot_json(eff: &Eff, slot: usize) -> Option<Value> {
    eff.vals.get(slot).and_then(|v| v.as_ref()).map(val_json)
}

/// Converts consecutive read float slots to a JSON array.
pub(crate) fn floats_json(eff: &Eff, from: usize, count: usize) -> CoreResult<Value> {
    (from..from + count)
        .map(|slot| eff.float(slot).map(float_json))
        .collect::<CoreResult<Vec<_>>>()
        .map(Value::Array)
}

pub(crate) fn ints_json(eff: &Eff, from: usize, count: usize) -> CoreResult<Value> {
    (from..from + count)
        .map(|slot| eff.int(slot).map(|v| json!(v)))
        .collect::<CoreResult<Vec<_>>>()
        .map(Value::Array)
}

/// Field access on a document object with typed errors.
pub(crate) fn field<'a>(value: &'a Value, key: &str) -> CoreResult<&'a Value> {
    value
        .get(key)
        .ok_or_else(|| fail(format!("missing field {key}")))
}

pub(crate) fn int_field(value: &Value, key: &str) -> CoreResult<Val> {
    Ok(Val::I(i32_of(field(value, key)?)?))
}

pub(crate) fn float_field(value: &Value, key: &str) -> CoreResult<Val> {
    Ok(Val::F(f32_of(field(value, key)?)?))
}

pub(crate) fn text_field(value: &Value, key: &str) -> CoreResult<Val> {
    Ok(Val::S(text_of_json(field(value, key)?)?))
}

/// Reads an optional field with a converter, giving an absent slot when the key is missing.
pub(crate) fn opt_field(
    value: &Value,
    key: &str,
    convert: fn(&Value) -> CoreResult<Val>,
) -> CoreResult<Option<Val>> {
    value.get(key).map(convert).transpose()
}

pub(crate) fn int_val(value: &Value) -> CoreResult<Val> {
    Ok(Val::I(i32_of(value)?))
}

pub(crate) fn float_val(value: &Value) -> CoreResult<Val> {
    Ok(Val::F(f32_of(value)?))
}

pub(crate) fn text_val(value: &Value) -> CoreResult<Val> {
    Ok(Val::S(text_of_json(value)?))
}

pub(crate) fn array<'a>(value: &'a Value, key: &str) -> CoreResult<&'a Vec<Value>> {
    field(value, key)?
        .as_array()
        .ok_or_else(|| fail(format!("field {key} is not an array")))
}

/// Reads a fixed-length float array field into slots.
pub(crate) fn float_slots(value: &Value, key: &str, count: usize) -> CoreResult<Vec<Option<Val>>> {
    let items = array(value, key)?;
    if items.len() != count {
        return Err(fail(format!("field {key} needs {count} numbers")));
    }
    items.iter().map(|v| Ok(Some(Val::F(f32_of(v)?)))).collect()
}

pub(crate) fn int_slots(value: &Value, key: &str, count: usize) -> CoreResult<Vec<Option<Val>>> {
    let items = array(value, key)?;
    if items.len() != count {
        return Err(fail(format!("field {key} needs {count} integers")));
    }
    items.iter().map(|v| Ok(Some(Val::I(i32_of(v)?)))).collect()
}

// --- Families --------------------------------------------------------------------------------------

/// A configuration family: the passes that read it and how its document is built and flattened.
pub(crate) struct Family {
    /// Document kind.
    pub kind: &'static str,
    /// Tag tables whose callbacks read the script, in the order their documents' values flatten.
    pub passes: &'static [&'static Pass],
    /// Commands present in retail scripts of this family that no consumer reads.
    pub ignored: &'static [&'static str],
    /// Builds the document body from each pass's effective values.
    pub build: fn(&str, &[Vec<Eff>]) -> CoreResult<Value>,
    /// Gives each pass's effective values back from a document.
    pub flatten: fn(&Value) -> CoreResult<Vec<Vec<Eff>>>,
    /// Whether the order between commands of different names matters; when it does not, the
    /// document groups each command's rows and the parity check compares them by name.
    pub ordered: bool,
}

/// Groups a pass's rows by command name, keeping each command's own order.
fn grouped(passes: &[Vec<Eff>]) -> Vec<BTreeMap<String, Vec<Eff>>> {
    passes
        .iter()
        .map(|rows| {
            let mut groups = BTreeMap::<String, Vec<Eff>>::new();
            for row in rows {
                groups
                    .entry(row.name.clone())
                    .or_default()
                    .push(row.clone());
            }
            groups
        })
        .collect()
}

fn families() -> Vec<&'static Family> {
    let mut all = Vec::new();
    all.extend(map::FAMILIES);
    all.extend(scene::FAMILIES);
    all.extend(dungeon::FAMILIES);
    all.extend(villagers::FAMILIES);
    all.extend(game::FAMILIES);
    all.extend(items::FAMILIES);
    all.extend(menu::FAMILIES);
    all.extend(records::FAMILIES);
    all
}

fn family_of_kind(kind: &str) -> Option<&'static Family> {
    families().into_iter().find(|f| f.kind == kind)
}

/// Retail scripts that no game code reads; they are consumed and nothing is published for them.
pub(crate) const UNUSED_RETAIL: &[&str] = &[
    "edit.cfg",
    "sg/omake0.cfg",
    "sg/omake1.cfg",
    "sg/fish/fish.cfg",
    "menu/cfg5/commentmes.cfg",
    "menu/cfg6/commentmes.cfg",
    "menu/cfg7/commentmes.cfg",
    "map/map.cfg",
];

/// Whether an alias is a retail script that no game code reads.
pub(crate) fn unused(alias: &str) -> bool {
    let lower = alias.to_ascii_lowercase();
    UNUSED_RETAIL.contains(&lower.as_str())
}

/// Chooses the family of a script from its path.
fn select(alias: &str) -> CoreResult<&'static Family> {
    let lower = alias.to_ascii_lowercase();
    let path = lower.split("#entry:").next().unwrap_or(&lower);
    let leaf = path.rsplit('/').next().unwrap_or(path);
    let kind = if path.ends_with(".map") {
        "scene_map"
    } else if leaf == "info.cfg" && path.contains(".pcp/") {
        "model_list"
    } else if leaf == "info.cfg"
        && (path == "map/s/s55/s55/info.cfg"
            || path
                .rsplit('/')
                .nth(1)
                .is_some_and(|dir| dir.ends_with(".sky") || dir.ends_with(".skyb")))
    {
        "sky"
    } else if let Some(kind) = dungeon::kind(path, leaf) {
        kind
    } else if let Some(kind) = villagers::kind(path, leaf) {
        kind
    } else if let Some(kind) = game::kind(path, leaf) {
        kind
    } else if let Some(kind) = items::kind(path, leaf) {
        kind
    } else if let Some(kind) = records::kind(path, leaf) {
        kind
    } else if let Some(kind) = menu::kind(path, leaf) {
        kind
    } else if path.starts_with("map/") {
        "scene_config"
    } else {
        return Err(fail(format!(
            "{alias}: no configuration family claims this script"
        )));
    };
    family_of_kind(kind).ok_or_else(|| fail(format!("{alias}: unknown family {kind}")))
}

/// Puts one pass's values in the order its family's documents give them back, where the family
/// stores commands that do not interact in separate tables.
fn canonical(kind: &str, pass: usize, rows: Vec<Eff>) -> Vec<Eff> {
    match (kind, pass) {
        ("scene_map", 0) => map::canonical_map_info(rows),
        ("scene_config", _) => scene::canonical_scene_config(rows),
        _ => rows,
    }
}

fn envelope(kind: &str, body: Value) -> CoreResult<Value> {
    let Value::Object(body) = body else {
        return Err(fail("family document body is not an object"));
    };
    let mut out = Map::new();
    out.insert("schema_version".into(), json!(SCHEMA_VERSION));
    out.insert("kind".into(), json!(kind));
    for (key, value) in body {
        out.insert(key, value);
    }
    Ok(Value::Object(out))
}

/// Builds the native document of one script from its v1 command list, proving that it flattens
/// back to the same callback values.
pub(crate) fn build(alias: &str, document: &Value) -> CoreResult<(Value, Report)> {
    let family = select(alias)?;
    let commands = sources(document)?;
    let mut report = Report::default();
    check_known(alias, family.passes, family.ignored, &commands, &mut report)?;
    let passes = family
        .passes
        .iter()
        .map(|pass| effective(pass, &commands, &mut report))
        .collect::<CoreResult<Vec<_>>>()
        .map_err(|e| fail(format!("{alias}: {e}")))?;
    let body = (family.build)(alias, &passes).map_err(|e| fail(format!("{alias}: {e}")))?;
    let native = envelope(family.kind, body)?;
    let flattened =
        (family.flatten)(&native).map_err(|e| fail(format!("{alias}: flatten: {e}")))?;
    let expected: Vec<Vec<Eff>> = passes
        .iter()
        .enumerate()
        .map(|(index, rows)| canonical(family.kind, index, rows.clone()))
        .collect();
    let same = if family.ordered {
        flattened == expected
    } else {
        grouped(&flattened) == grouped(&expected)
    };
    if !same {
        return Err(fail(format!(
            "{alias}: {} document does not reproduce its script ({})",
            family.kind,
            first_difference(&expected, &flattened)
        )));
    }
    Ok((native, report))
}

fn first_difference(expected: &[Vec<Eff>], actual: &[Vec<Eff>]) -> String {
    for (pass, (e, a)) in expected.iter().zip(actual).enumerate() {
        for (row, (x, y)) in e.iter().zip(a).enumerate() {
            if x != y {
                return format!("pass {pass} row {row}: {x:?} became {y:?}");
            }
        }
        if e.len() != a.len() {
            return format!("pass {pass}: {} rows became {}", e.len(), a.len());
        }
    }
    if expected.len() != actual.len() {
        return format!("{} passes became {}", expected.len(), actual.len());
    }
    "no difference".into()
}

/// Gives each pass's callback values back from a native document.
pub(crate) fn flatten(document: &Value) -> CoreResult<Vec<Vec<Eff>>> {
    let kind = document["kind"]
        .as_str()
        .ok_or_else(|| fail("document kind missing"))?;
    let family =
        family_of_kind(kind).ok_or_else(|| fail(format!("unknown document kind {kind}")))?;
    (family.flatten)(document)
}

/// Validates a published configuration document's envelope and structure.
pub(crate) fn validate(document: &Value) -> CoreResult<()> {
    if document.get("commands").is_some() {
        return Err(fail("configuration document still carries commands"));
    }
    if document["schema_version"] != SCHEMA_VERSION {
        return Err(fail("unsupported configuration document schema"));
    }
    let kind = document["kind"]
        .as_str()
        .ok_or_else(|| fail("configuration document kind missing"))?;
    if kind == "movie_subtitles" {
        return Ok(());
    }
    flatten(document).map(|_| ())
}

// --- Shared building blocks ----------------------------------------------------------------------

/// Walks effective values in order, giving each to the family builder.
pub(crate) struct Cursor<'a> {
    rows: &'a [Eff],
    at: usize,
}

impl<'a> Cursor<'a> {
    pub fn new(rows: &'a [Eff]) -> Cursor<'a> {
        Cursor { rows, at: 0 }
    }
    pub fn peek(&self) -> Option<&'a Eff> {
        self.rows.get(self.at)
    }
    pub fn peek_name(&self) -> Option<&'a str> {
        self.peek().map(|e| e.name.as_str())
    }
    pub fn next(&mut self) -> Option<&'a Eff> {
        let row = self.rows.get(self.at);
        if row.is_some() {
            self.at += 1;
        }
        row
    }
    /// Takes the next row when it has the given name.
    pub fn take(&mut self, name: &str) -> Option<&'a Eff> {
        if self.peek_name() == Some(name) {
            self.next()
        } else {
            None
        }
    }
    /// Takes the next row, which must have the given name.
    pub fn expect(&mut self, name: &str) -> CoreResult<&'a Eff> {
        self.take(name).ok_or_else(|| {
            fail(format!(
                "expected {name}, found {}",
                self.peek_name().unwrap_or("the end")
            ))
        })
    }
    pub fn done(&self) -> bool {
        self.at >= self.rows.len()
    }
    pub fn unexpected(&self) -> CoreError {
        fail(format!(
            "unexpected {} at row {}",
            self.peek_name().unwrap_or("end"),
            self.at
        ))
    }
}

/// Converts every read slot of a row to JSON, in slot order, for families whose rows are records.
pub(crate) fn row_json(eff: &Eff, keys: &[&str]) -> Value {
    let mut obj = Obj::new();
    for (slot, key) in keys.iter().enumerate() {
        obj = obj.opt(key, slot_json(eff, slot));
    }
    obj.done()
}

/// Inverse of `row_json` given each key's read type.
pub(crate) fn row_of(value: &Value, name: &str, keys: &[(&str, char)]) -> CoreResult<Eff> {
    let mut vals = Vec::new();
    for (key, ty) in keys {
        let convert: fn(&Value) -> CoreResult<Val> = match ty {
            'i' => int_val,
            'f' => float_val,
            's' => text_val,
            _ => return Err(fail("bad row key type")),
        };
        vals.push(opt_field(value, key, convert)?);
    }
    Ok(Eff::new(name, vals))
}

#[cfg(test)]
mod tests {
    use super::*;

    const PASS: Pass = Pass {
        name: "test",
        commands: &[
            Command {
                name: "GRP",
                layout: "i i i i",
            },
            Command {
                name: "ROW",
                layout: "i f? s?",
            },
            Command {
                name: "LIST",
                layout: "i *i",
            },
            Command {
                name: "NOP",
                layout: "-",
            },
        ],
    };

    fn run(script: &[u8]) -> CoreResult<(Vec<Eff>, Report)> {
        let document = super::super::configs::parse(script)?;
        let mut report = Report::default();
        let rows = effective(&PASS, &sources(&document)?, &mut report)?;
        Ok((rows, report))
    }

    #[test]
    fn coercion_matches_the_stack_readers() {
        let (rows, report) = run(b"ROW 2.7,3,\"t\"; ROW x,y; UNKNOWN 9;").unwrap();
        assert_eq!(rows[0].vals[0], Some(Val::I(2)));
        assert_eq!(rows[0].vals[1], Some(Val::F(3.0)));
        assert_eq!(rows[1].vals[0], Some(Val::I(0)));
        assert_eq!(rows[1].vals[1], Some(Val::F(0.0)));
        assert_eq!(rows[1].vals[2], None);
        assert!(report.stale.is_empty());
    }

    #[test]
    fn short_rows_read_the_previous_commands_slots() {
        let (rows, report) = run(b"GRP 1,2,3,4; NOP 5; GRP 7,8;").unwrap();
        assert_eq!(
            rows[1].vals,
            vec![
                Some(Val::I(7)),
                Some(Val::I(8)),
                Some(Val::I(3)),
                Some(Val::I(4))
            ]
        );
        assert_eq!(report.stale.len(), 2);
        // An unknown command does not push its operands.
        let (rows, _) = run(b"GRP 1,2,3,4; OTHER 9,9,9,9; GRP 7;").unwrap();
        assert_eq!(rows[1].vals[1], Some(Val::I(2)));
    }

    #[test]
    fn rest_slots_and_noops() {
        let (rows, _) = run(b"LIST 1,2,3; NOP; LIST 4;").unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].vals.len(), 3);
        assert_eq!(rows[1].vals.len(), 1);
    }

    #[test]
    fn stale_strings_are_refused() {
        let pass = Pass {
            name: "s",
            commands: &[Command {
                name: "S",
                layout: "s s",
            }],
        };
        let document = super::super::configs::parse(b"S \"a\",\"b\"; S \"c\";").unwrap();
        let error = effective(&pass, &sources(&document).unwrap(), &mut Report::default())
            .unwrap_err()
            .to_string();
        assert!(error.contains("stale string"), "{error}");
    }

    #[test]
    fn floats_serialize_shortest_and_round_trip() {
        for value in [0.2f32, 111.23, -860.131, 1.0, 1e-7, 3.4e38] {
            let json = float_json(value);
            assert_eq!(f32_of(&json).unwrap().to_bits(), value.to_bits());
        }
        assert_eq!(float_json(0.2).to_string(), "0.2");
        assert_eq!(float_json(1.0).to_string(), "1.0");
    }

    #[test]
    fn every_family_kind_is_unique() {
        let mut kinds: Vec<_> = families().iter().map(|f| f.kind).collect();
        kinds.sort();
        let count = kinds.len();
        kinds.dedup();
        assert_eq!(kinds.len(), count);
    }
}
