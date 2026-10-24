//! Converts retail scene, mesh, skeleton and animation records into glTF and typed JSON.
use super::actions::{KeyAction, NameBook};
use super::artifacts::{ArtifactBytes, AssetSink, Category, Job};
use super::progress::Phase;
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

fn fail(s: impl Into<String>) -> CoreError {
    CoreError(s.into())
}
fn bytes(b: &[u8], at: usize, n: usize) -> CoreResult<&[u8]> {
    b.get(
        at..at
            .checked_add(n)
            .ok_or_else(|| fail("model range overflow"))?,
    )
    .ok_or_else(|| fail(format!("model range {at}+{n} exceeds {}", b.len())))
}
fn u32le(b: &[u8], at: usize) -> CoreResult<u32> {
    Ok(u32::from_le_bytes(bytes(b, at, 4)?.try_into().unwrap()))
}
fn i32le(b: &[u8], at: usize) -> CoreResult<i32> {
    Ok(u32le(b, at)? as i32)
}
fn floats(b: &[u8], at: usize, n: usize) -> CoreResult<Vec<f32>> {
    bytes(b, at, n * 4)?
        .as_chunks::<4>()
        .0
        .iter()
        .map(|x| {
            let f = f32::from_le_bytes(*x);
            if f.is_finite() {
                Ok(f)
            } else {
                Err(fail("nonfinite model value"))
            }
        })
        .collect()
}
fn name(b: &[u8]) -> String {
    let end = b.iter().position(|&v| v == 0).unwrap_or(b.len());
    b[..end]
        .iter()
        .map(|&v| {
            if (32..127).contains(&v) {
                char::from(v).to_string()
            } else {
                format!("%{v:02X}")
            }
        })
        .collect()
}
fn base64(b: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(b.len().div_ceil(3) * 4);
    for c in b.chunks(3) {
        let x = ((c[0] as u32) << 16)
            | ((c.get(1).copied().unwrap_or(0) as u32) << 8)
            | c.get(2).copied().unwrap_or(0) as u32;
        out.push(T[(x >> 18) as usize] as char);
        out.push(T[((x >> 12) & 63) as usize] as char);
        out.push(if c.len() > 1 {
            T[((x >> 6) & 63) as usize] as char
        } else {
            '='
        });
        out.push(if c.len() > 2 {
            T[(x & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}
/// Writes a generated model JSON asset without indentation.
fn save(path: &Path, document: &Value) -> CoreResult<()> {
    fs::create_dir_all(path.parent().ok_or_else(|| fail("missing model parent"))?)?;
    fs::write(path, serde_json::to_vec(document)?)?;
    Ok(())
}
fn is_compiled_script_path(path: &str) -> bool {
    !path.contains([':', '\0', '\\'])
        && Path::new(path)
            .components()
            .all(|component| matches!(component, std::path::Component::Normal(_)))
        && (path.to_ascii_lowercase().ends_with(".stb")
            || path.to_ascii_lowercase().ends_with(".stb_"))
}

fn may_omit_character_resource(key: &str, path: &str, publish_scripts: bool) -> bool {
    key == "scripts" && !publish_scripts && is_compiled_script_path(path)
}

fn extension(alias: &str) -> &str {
    alias
        .split("#entry:")
        .next()
        .unwrap_or(alias)
        .rsplit('.')
        .next()
        .unwrap_or("")
}
fn stem(alias: &str) -> &str {
    alias.rsplit_once('.').map_or(alias, |p| p.0)
}
fn is_character(alias: &str) -> bool {
    extension(alias)
        .trim_end_matches('_')
        .eq_ignore_ascii_case("chr")
}

/// Native profile names available to script migration before graphics jobs run.
pub(crate) fn planned_profile_names(
    inventory: &super::inventory::Inventory,
) -> CoreResult<BTreeMap<String, BTreeSet<String>>> {
    let mut names = BTreeMap::new();
    let profiled: BTreeSet<&str> = inventory
        .names()
        .filter(|resource| extension(resource).eq_ignore_ascii_case("cfg"))
        .filter_map(character_parent)
        .collect();
    for alias in inventory.names() {
        if !is_character(alias) {
            continue;
        }
        if let Some(plan) = super::configs::profile_actions_for(alias)? {
            names.insert(
                alias.to_owned(),
                plan.profiles.iter().map(|alias| leaf_stem(alias)).collect(),
            );
        } else if profiled.contains(alias) {
            return Err(fail(format!(
                "missing checked-in gameplay clip plan for {alias}"
            )));
        } else {
            names.insert(alias.to_owned(), BTreeSet::new());
        }
    }
    Ok(names)
}
fn parent(alias: &str) -> &str {
    alias.rsplit_once('/').map_or("", |p| p.0)
}
fn output_path(alias: &str, suffix: &str) -> String {
    let clean: String = alias
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || matches!(c, '/' | '_' | '-' | '.') {
                c
            } else {
                '_'
            }
        })
        .collect();
    format!("models/{clean}.{suffix}")
}

#[derive(Clone)]
struct Resource {
    alias: String,
    data: Vec<u8>,
}
#[derive(Clone)]
pub(crate) struct Member {
    pub(crate) name: String,
    pub(crate) start: usize,
    pub(crate) size: usize,
    size_field: usize,
}
pub(crate) fn pack(b: &[u8]) -> Option<Vec<Member>> {
    if super::native_scripts::is_valid_script(b) {
        return None;
    }
    let mut out = Vec::new();
    let mut at = 0;
    let mut seen = BTreeSet::new();
    while seen.insert(at) && out.len() < 16384 {
        let h = b.get(at..at + 76)?;
        let end = h[..64].iter().position(|&v| v == 0)?;
        if end == 0 {
            return if out.is_empty() { None } else { Some(out) };
        }
        if h[..end]
            .iter()
            .any(|v| *v < 32 || matches!(*v, b'/' | b'\\'))
        {
            return None;
        }
        let offset = u32le(h, 64).ok()? as usize;
        let size = u32le(h, 68).ok()? as usize;
        let advance = u32le(h, 72).ok()? as usize;
        if offset < 76 || advance < 76 || at.checked_add(offset)?.checked_add(size)? > b.len() {
            return None;
        }
        out.push(Member {
            name: name(&h[..end]),
            start: at + offset,
            size,
            size_field: at + 68,
        });
        at = at.checked_add(advance)?;
    }
    None
}
fn mesh_extent(b: &[u8]) -> CoreResult<usize> {
    if !b.starts_with(b"MDT\0") {
        return Err(fail("invalid MDT signature"));
    }
    bytes(b, 0, 64)?;
    let mut end = 64;
    for (count, offset, stride) in [
        (12, 16, 16),
        (20, 24, 16),
        (28, 32, 16),
        (36, 40, 1),
        (44, 48, 16),
        (52, 56, 96),
    ] {
        let n = u32le(b, count)? as usize;
        let at = u32le(b, offset)? as usize;
        if n > 0 {
            bytes(
                b,
                at,
                n.checked_mul(stride)
                    .ok_or_else(|| fail("MDT count overflow"))?,
            )?;
            end = end.max(at + n * stride);
        }
    }
    Ok(end)
}
fn scene_extent(b: &[u8]) -> CoreResult<usize> {
    if !b.starts_with(b"MDS\0") {
        return Err(fail("invalid MDS signature"));
    }
    let count = u32le(b, 8)? as usize;
    let mut at = u32le(b, 12)? as usize;
    let mut end = 16;
    if count > 16384 || at < 16 {
        return Err(fail("invalid MDS objects"));
    }
    for _ in 0..count {
        bytes(b, at, 112)?;
        let size = u32le(b, at + 4)? as usize;
        if size < 112 {
            return Err(fail("invalid MDS object stride"));
        }
        let m = u32le(b, at + 40)? as usize;
        if m > 0 {
            end = end.max(m + mesh_extent(bytes(b, m, b.len() - m)?)?)
        }
        at = at
            .checked_add(size)
            .ok_or_else(|| fail("MDS object overflow"))?;
        end = end.max(at);
    }
    bytes(b, 0, end)?;
    Ok(end)
}
fn discover(b: &[u8], alias: &str, resources: &mut Vec<Resource>, depth: usize) -> CoreResult<()> {
    if depth > 16 {
        return Err(fail("model PACK nesting exceeds 16"));
    }
    if let Some(members) = pack(b) {
        if is_character(alias) {
            resources.push(Resource {
                alias: alias.into(),
                data: b.to_vec(),
            });
        }
        let mut names = BTreeSet::new();
        for (i, m) in members.iter().enumerate() {
            if m.size == 0 {
                continue;
            }
            let child = if names.insert(m.name.clone()) {
                format!("{alias}/{}", m.name)
            } else {
                format!("{alias}/{}#entry:{i}", m.name)
            };
            discover(bytes(b, m.start, m.size)?, &child, resources, depth + 1)?;
        }
        return Ok(());
    }
    let ext = extension(alias).to_ascii_lowercase();
    if is_character(alias) && !b.is_empty() {
        return Err(fail(format!("character is not a valid PACK: {alias}")));
    }
    if matches!(
        ext.as_str(),
        "mds"
            | "mdt"
            | "mot"
            | "wgt"
            | "bbp"
            | "cfg"
            | "clo"
            | "chr"
            | "chr_"
            | "img"
            | "tm2"
            | "stb"
            | "ico"
    ) {
        resources.push(Resource {
            alias: alias.into(),
            data: b.to_vec(),
        });
        if !matches!(ext.as_str(), "cfg" | "clo") {
            return Ok(());
        }
    }
    let mut at = 0;
    while at + 4 <= b.len() {
        let candidate = &b[at..];
        let extent = if candidate.starts_with(b"MDS\0") {
            scene_extent(candidate).ok()
        } else if candidate.starts_with(b"MDT\0") {
            mesh_extent(candidate).ok()
        } else {
            None
        };
        if let Some(size) = extent {
            resources.push(Resource {
                alias: format!("{alias}#offset:{at}"),
                data: b[at..at + size].to_vec(),
            });
            at += size;
        } else {
            at += 1;
        }
    }
    Ok(())
}
fn scrub(
    b: &mut [u8],
    alias: &str,
    converted: &BTreeSet<String>,
    depth: usize,
) -> CoreResult<bool> {
    if depth > 16 {
        return Err(fail("model PACK nesting exceeds 16"));
    }
    if let Some(members) = pack(b) {
        let mut changed = false;
        let mut names = BTreeSet::new();
        for (i, m) in members.iter().enumerate() {
            let child = if names.insert(m.name.clone()) {
                format!("{alias}/{}", m.name)
            } else {
                format!("{alias}/{}#entry:{i}", m.name)
            };
            if converted.contains(&child) {
                b[m.start..m.start + m.size].fill(0);
                b[m.size_field..m.size_field + 4].fill(0);
                changed = true;
            } else if m.size > 0 {
                changed |= scrub(
                    &mut b[m.start..m.start + m.size],
                    &child,
                    converted,
                    depth + 1,
                )?;
            }
        }
        return Ok(changed);
    }
    let mut changed = false;
    let prefix = format!("{alias}#offset:");
    for key in converted.range(prefix.clone()..) {
        let Some(offset) = key.strip_prefix(&prefix) else {
            break;
        };
        if let Ok(at) = offset.parse::<usize>() {
            let size = if b[at..].starts_with(b"MDS\0") {
                scene_extent(&b[at..])?
            } else {
                mesh_extent(&b[at..])?
            };
            b[at..at + size].fill(0);
            changed = true;
        }
    }
    Ok(changed)
}

fn channels(resource: &Resource) -> CoreResult<Vec<Value>> {
    let b = &resource.data;
    let mut at = 0;
    let mut out = Vec::new();
    if b.is_empty() {
        return Ok(out);
    }
    loop {
        bytes(b, at, 32)?;
        let count = u32le(b, at + 16)? as usize;
        let more = u32le(b, at + 20)?;
        if count > 10_000_000 {
            return Err(fail("excessive motion keys"));
        }
        bytes(
            b,
            at + 32,
            count
                .checked_mul(32)
                .ok_or_else(|| fail("motion count overflow"))?,
        )?;
        let mut keys = Vec::with_capacity(count);
        for i in 0..count {
            let p = at + 32 + i * 32;
            keys.push(json!({"frame":u32le(b,p)?,"value":floats(b,p+16,4)?,"reserved_u32":[u32le(b,p+4)?,u32le(b,p+8)?,u32le(b,p+12)?]}));
        }
        out.push(json!({"alias":resource.alias,"frame":i32le(b,at)?,"target":i32le(b,at+4)?,"type":i32le(b,at+8)?,"reserved_u32":[u32le(b,at+12)?,u32le(b,at+24)?,u32le(b,at+28)?],"keys":keys}));
        at += 32 + count * 32;
        if more == 0 {
            break;
        }
        if out.len() > 100000 {
            return Err(fail("excessive motion channels"));
        }
    }
    if b[at..].iter().any(|&v| v != 0) {
        return Err(fail(format!(
            "unparsed motion trailing data: {}",
            resource.alias
        )));
    }
    Ok(out)
}
fn bind_pose(resource: &Resource) -> CoreResult<Value> {
    if !resource.data.len().is_multiple_of(64) {
        return Err(fail(format!("invalid bind pose: {}", resource.alias)));
    }
    let matrices: CoreResult<Vec<_>> = (0..resource.data.len() / 64)
        .map(|i| floats(&resource.data, i * 64, 16))
        .collect();
    Ok(json!({"alias":resource.alias,"matrices":matrices?}))
}

fn script_links(resources: &[Resource]) -> BTreeMap<String, BTreeSet<String>> {
    let mut links = BTreeMap::<String, BTreeSet<String>>::new();
    for r in resources
        .iter()
        .filter(|r| matches!(extension(&r.alias), "cfg" | "clo"))
    {
        let mut model = String::new();
        let mut shadow = String::new();
        for statement in r.data.split(|&b| b == b';') {
            let line = statement
                .rsplit(|&b| b == b'\n')
                .next()
                .unwrap_or(statement)
                .trim_ascii();
            let command = line
                .split(|b| b.is_ascii_whitespace())
                .next()
                .unwrap_or(&[]);
            let quoted: Vec<_> = line
                .split(|&b| b == b'"')
                .enumerate()
                .filter(|(i, _)| i % 2 == 1)
                .map(|(_, b)| name(b))
                .collect();
            match command {
                b"MODEL" => {
                    if let Some(value) = quoted.first() {
                        model = format!("{}/{}", parent(&r.alias), value)
                    }
                }
                b"SHADOW_MODEL" => {
                    if let Some(value) = quoted.first() {
                        shadow = format!("{}/{}", parent(&r.alias), value)
                    }
                }
                b"MOTION" | b"SHADOW_MOTION" => {
                    let target = if command == b"MOTION" {
                        &model
                    } else {
                        &shadow
                    };
                    if !target.is_empty() {
                        for value in quoted.iter().filter(|v| !v.is_empty()) {
                            links.entry(target.clone()).or_default().insert(format!(
                                "{}/{}",
                                parent(&r.alias),
                                value
                            ));
                        }
                    }
                }
                _ => {}
            }
        }
    }
    links
}

struct Gltf {
    doc: Value,
    buffer: Vec<u8>,
    /// Shear helper child of each node whose retail scale-before-rotation pose has shear.
    rotation_nodes: BTreeMap<usize, usize>,
    /// Shear helpers whose authored scale resets when retail rotation keys replace the matrix.
    reset_rotation_scale: BTreeSet<usize>,
    /// Nodes whose rotation and scale clips are rebaked from the retail pose at every frame.
    baked: BTreeSet<usize>,
    /// Authored matrix of each node a clip has split into translation, rotation and scale.
    authored: BTreeMap<usize, Vec<f32>>,
    /// Transform conventions and boundary keys counted for the conversion report.
    stats: TransformStats,
    /// Animation accessors by their exact contents, so identical key data is stored once.
    shared: BTreeMap<(Vec<u32>, usize, bool), usize>,
    /// Animation key data, stored after the rest of the buffer in one buffer view.
    animation_bytes: Vec<u8>,
    /// Accessors whose data lies in the animation buffer view.
    animation_accessors: Vec<usize>,
}

/// Counts how animated retail transforms were expressed in standard glTF.
#[derive(Default, Clone, Debug)]
pub struct TransformStats {
    /// Animated frames whose retail keys are standard translation, rotation and scale keys.
    pub plain: usize,
    /// Frames whose rotation and scale are rebaked from the retail pose at every game frame.
    pub baked: usize,
    /// Largest difference between a rebaked pose and the retail pose between rebaked keys.
    pub baked_error: f32,
    /// Frames that keep a shear helper node, with the reason.
    pub helpers: Vec<String>,
    /// Retail-sampled keys added at action boundaries.
    pub boundary_keys: usize,
}
impl TransformStats {
    fn add(&mut self, other: &TransformStats) {
        self.plain += other.plain;
        self.baked += other.baked;
        self.baked_error = self.baked_error.max(other.baked_error);
        self.helpers.extend(other.helpers.iter().cloned());
        self.boundary_keys += other.boundary_keys;
    }
}
/// Converts a memory card icon model into glTF referencing `image_uri` and the PNG it names.
fn memory_card_icon(data: &[u8], image_uri: &str) -> CoreResult<(Value, Vec<u8>)> {
    let shapes = u32le(data, 4)? as usize;
    let count = u32le(data, 16)? as usize;
    if u32le(data, 0)? != 0x10000
        || shapes != 1
        || count == 0
        || !count.is_multiple_of(3)
        || count > 1_000_000
    {
        return Err(fail("unsupported memory card icon model"));
    }
    let stride = shapes * 8 + 16;
    bytes(
        data,
        20,
        count
            .checked_mul(stride)
            .ok_or_else(|| fail("icon vertex overflow"))?,
    )?;
    let mut positions = Vec::new();
    let mut normals = Vec::new();
    let mut uvs = Vec::new();
    let mut colours = Vec::new();
    let signed = |at| -> CoreResult<f32> {
        Ok(i16::from_le_bytes(bytes(data, at, 2)?.try_into().unwrap()) as f32 / 4096.)
    };
    for vertex in 0..count {
        let at = 20 + vertex * stride;
        for component in 0..3 {
            positions.push(signed(at + component * 2)? * if component == 0 { 1. } else { -1. });
        }
        let mut normal = Vec::new();
        for component in 0..3 {
            normal.push(signed(at + 8 + component * 2)? * if component == 0 { 1. } else { -1. });
        }
        let length = normal.iter().map(|v| v * v).sum::<f32>().sqrt();
        if length > 1e-20 {
            for value in &mut normal {
                *value /= length;
            }
        } else {
            normal = vec![0., 1., 0.];
        }
        normals.extend(normal);
        uvs.extend([signed(at + 16)?, signed(at + 18)?]);
        colours.extend(bytes(data, at + 20, 4)?.iter().map(|v| *v as f32 / 255.));
    }
    let mut at = 20 + count * stride;
    let frames = u32le(data, at + 16)? as usize;
    floats(data, at + 8, 1)?;
    at += 20;
    if frames > 1024 {
        return Err(fail("excessive icon animation frames"));
    }
    for _ in 0..frames {
        let shape = u32le(data, at)?;
        let keys = u32le(data, at + 4)? as usize;
        at += 8;
        if shape != 0 || keys > 1024 {
            return Err(fail("unsupported animated icon shape"));
        }
        for _ in 0..keys {
            let key = floats(data, at, 2)?;
            if key[1] != 1. {
                return Err(fail("unsupported changing icon shape weight"));
            }
            at += 8;
        }
    }
    let mut texels = Vec::new();
    if u32le(data, 8)? & 8 != 0 {
        let length = u32le(data, at)? as usize;
        at += 4;
        let end = at
            .checked_add(length)
            .ok_or_else(|| fail("icon texture overflow"))?;
        bytes(data, at, length)?;
        while at < end && texels.len() < 128 * 128 {
            let count = u16::from_le_bytes(bytes(data, at, 2)?.try_into().unwrap());
            at += 2;
            if count & 0x8000 != 0 {
                let count = 0x10000 - count as usize;
                for _ in 0..count {
                    texels.push(u16::from_le_bytes(bytes(data, at, 2)?.try_into().unwrap()));
                    at += 2;
                }
            } else {
                let value = u16::from_le_bytes(bytes(data, at, 2)?.try_into().unwrap());
                at += 2;
                texels.extend(std::iter::repeat_n(value, count as usize));
            }
            if at > end || texels.len() > 128 * 128 {
                return Err(fail("invalid icon texture run"));
            }
        }
        if at != end {
            return Err(fail("trailing icon texture runs"));
        }
    } else {
        for _ in 0..128 * 128 {
            texels.push(u16::from_le_bytes(bytes(data, at, 2)?.try_into().unwrap()));
            at += 2;
        }
    }
    if texels.len() != 128 * 128 {
        return Err(fail("incomplete icon texture"));
    }
    let mut rgba = Vec::with_capacity(128 * 128 * 4);
    for colour in texels {
        rgba.extend([
            ((colour & 31) << 3) as u8,
            (((colour >> 5) & 31) << 3) as u8,
            (((colour >> 10) & 31) << 3) as u8,
            255,
        ]);
    }
    let mut png = Vec::new();
    {
        let mut encoder = png::Encoder::new(&mut png, 128, 128);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        let mut writer = encoder
            .write_header()
            .map_err(|e| fail(format!("icon PNG: {e}")))?;
        writer
            .write_image_data(&rgba)
            .map_err(|e| fail(format!("icon PNG: {e}")))?;
    }
    let mut gltf = Gltf::new();
    gltf.doc["asset"]["generator"] = json!("Kazarov Memory Card Icon");
    let position = gltf.accessor(&positions, 3, "VEC3", true);
    let normal = gltf.accessor(&normals, 3, "VEC3", false);
    let uv = gltf.accessor(&uvs, 2, "VEC2", false);
    let colour = gltf.accessor(&colours, 4, "VEC4", false);
    gltf.doc["images"] = json!([{"uri":image_uri}]);
    gltf.doc["textures"] = json!([{"source":0}]);
    gltf.doc["materials"] = json!([{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":1},"doubleSided":true}]);
    gltf.doc["meshes"] = json!([{"primitives":[{"attributes":{"POSITION":position,"NORMAL":normal,"TEXCOORD_0":uv,"COLOR_0":colour},"mode":4,"material":0}]}]);
    gltf.doc["nodes"] = json!([{"name":"memory_card_icon","mesh":0}]);
    gltf.doc["scenes"][0]["nodes"] = json!([0]);
    Ok((gltf.finish(), png))
}
impl Gltf {
    fn new() -> Self {
        Self {
            doc: json!({"asset":{"version":"2.0","generator":"Kazarov"},"scene":0,"scenes":[{"nodes":[]}],"nodes":[],"meshes":[],"materials":[],"accessors":[],"bufferViews":[],"animations":[]}),
            buffer: Vec::new(),
            rotation_nodes: BTreeMap::new(),
            reset_rotation_scale: BTreeSet::new(),
            baked: BTreeSet::new(),
            authored: BTreeMap::new(),
            stats: TransformStats::default(),
            shared: BTreeMap::new(),
            animation_bytes: Vec::new(),
            animation_accessors: Vec::new(),
        }
    }
    fn accessor(&mut self, values: &[f32], width: usize, kind: &str, bounds: bool) -> usize {
        while !self.buffer.len().is_multiple_of(4) {
            self.buffer.push(0)
        }
        let offset = self.buffer.len();
        for f in values {
            self.buffer.extend_from_slice(&f.to_le_bytes())
        }
        let views = self.doc["bufferViews"].as_array_mut().unwrap();
        let view = views.len();
        views.push(json!({"buffer":0,"byteOffset":offset,"byteLength":values.len()*4}));
        let mut a =
            json!({"bufferView":view,"componentType":5126,"count":values.len()/width,"type":kind});
        if bounds && !values.is_empty() {
            let mut min = vec![f32::INFINITY; width];
            let mut max = vec![f32::NEG_INFINITY; width];
            for v in values.chunks_exact(width) {
                for i in 0..width {
                    min[i] = min[i].min(v[i]);
                    max[i] = max[i].max(v[i]);
                }
            }
            a["min"] = json!(min);
            a["max"] = json!(max);
        }
        let accessors = self.doc["accessors"].as_array_mut().unwrap();
        let id = accessors.len();
        accessors.push(a);
        id
    }
    /// Stores animation key data once per file, sharing identical times and values between clips.
    fn animation_accessor(&mut self, values: &[f32], width: usize, bounds: bool) -> usize {
        let key = (values.iter().map(|v| v.to_bits()).collect(), width, bounds);
        if let Some(&id) = self.shared.get(&key) {
            return id;
        }
        let kind = match width {
            1 => "SCALAR",
            3 => "VEC3",
            _ => "VEC4",
        };
        let offset = self.animation_bytes.len();
        for f in values {
            self.animation_bytes.extend_from_slice(&f.to_le_bytes())
        }
        let mut a = json!({"bufferView":null,"byteOffset":offset,"componentType":5126,"count":values.len()/width,"type":kind});
        if bounds && !values.is_empty() {
            let mut min = vec![f32::INFINITY; width];
            let mut max = vec![f32::NEG_INFINITY; width];
            for v in values.chunks_exact(width) {
                for i in 0..width {
                    min[i] = min[i].min(v[i]);
                    max[i] = max[i].max(v[i]);
                }
            }
            a["min"] = json!(min);
            a["max"] = json!(max);
        }
        let accessors = self.doc["accessors"].as_array_mut().unwrap();
        let id = accessors.len();
        accessors.push(a);
        self.animation_accessors.push(id);
        self.shared.insert(key, id);
        id
    }
    /// Reads back an accessor's floats, wherever its data lies.
    #[cfg(test)]
    fn accessor_floats(&self, accessor: usize) -> Vec<f32> {
        let a = &self.doc["accessors"][accessor];
        let count = a["count"].as_u64().unwrap() as usize
            * match a["type"].as_str().unwrap() {
                "SCALAR" => 1,
                "VEC3" => 3,
                "VEC4" => 4,
                _ => 16,
            };
        if a["bufferView"].is_null() {
            return floats(
                &self.animation_bytes,
                a["byteOffset"].as_u64().unwrap() as usize,
                count,
            )
            .unwrap();
        }
        let view = &self.doc["bufferViews"][a["bufferView"].as_u64().unwrap() as usize];
        floats(
            &self.buffer,
            view["byteOffset"].as_u64().unwrap() as usize,
            view["byteLength"].as_u64().unwrap() as usize / 4,
        )
        .unwrap()
    }
    fn joints(&mut self, values: &[u16]) -> usize {
        while !self.buffer.len().is_multiple_of(4) {
            self.buffer.push(0)
        }
        let offset = self.buffer.len();
        for v in values {
            self.buffer.extend_from_slice(&v.to_le_bytes())
        }
        let views = self.doc["bufferViews"].as_array_mut().unwrap();
        let view = views.len();
        views.push(json!({"buffer":0,"byteOffset":offset,"byteLength":values.len()*2}));
        let a = self.doc["accessors"].as_array_mut().unwrap();
        let id = a.len();
        a.push(
            json!({"bufferView":view,"componentType":5123,"count":values.len()/4,"type":"VEC4"}),
        );
        id
    }
    fn sparse_axis(&mut self, count: usize, indices: &[u32], axis: usize) -> usize {
        while !self.buffer.len().is_multiple_of(4) {
            self.buffer.push(0)
        }
        let index_offset = self.buffer.len();
        for i in indices {
            self.buffer.extend_from_slice(&i.to_le_bytes())
        }
        let index_view = self.doc["bufferViews"].as_array().unwrap().len();
        self.doc["bufferViews"]
            .as_array_mut()
            .unwrap()
            .push(json!({"buffer":0,"byteOffset":index_offset,"byteLength":indices.len()*4}));
        let value_offset = self.buffer.len();
        for _ in indices {
            for c in 0..3 {
                self.buffer
                    .extend_from_slice(&(if c == axis { 1f32 } else { 0f32 }).to_le_bytes())
            }
        }
        let value_view = self.doc["bufferViews"].as_array().unwrap().len();
        self.doc["bufferViews"]
            .as_array_mut()
            .unwrap()
            .push(json!({"buffer":0,"byteOffset":value_offset,"byteLength":indices.len()*12}));
        let id = self.doc["accessors"].as_array().unwrap().len();
        self.doc["accessors"].as_array_mut().unwrap().push(json!({"componentType":5126,"count":count,"type":"VEC3","min":[0,0,0],"max":(0..3).map(|i|if i==axis{1}else{0}).collect::<Vec<_>>(),"sparse":{"count":indices.len(),"indices":{"bufferView":index_view,"componentType":5125},"values":{"bufferView":value_view}}}));
        id
    }
    fn finish(mut self) -> Value {
        if !self.animation_bytes.is_empty() {
            while !self.buffer.len().is_multiple_of(4) {
                self.buffer.push(0)
            }
            let views = self.doc["bufferViews"].as_array_mut().unwrap();
            let view = views.len();
            views.push(json!({"buffer":0,"byteOffset":self.buffer.len(),"byteLength":self.animation_bytes.len()}));
            self.buffer.extend_from_slice(&self.animation_bytes);
            for &accessor in &self.animation_accessors {
                self.doc["accessors"][accessor]["bufferView"] = json!(view);
            }
        }
        if !self.buffer.is_empty() {
            self.doc["buffers"] = json!([{"byteLength":self.buffer.len(),"uri":format!("data:application/octet-stream;base64,{}",base64(&self.buffer))}]);
        }
        for key in [
            "meshes",
            "materials",
            "accessors",
            "bufferViews",
            "animations",
            "skins",
            "images",
            "textures",
            "samplers",
        ] {
            if self.doc[key].as_array().is_some_and(|a| a.is_empty()) {
                self.doc.as_object_mut().unwrap().remove(key);
            }
        }
        if self.doc["nodes"]
            .as_array()
            .is_some_and(|nodes| nodes.is_empty())
        {
            for key in ["nodes", "scenes", "scene"] {
                self.doc.as_object_mut().unwrap().remove(key);
            }
        }
        self.doc
    }
}

/// A named texture entry from a converted texture document.
struct Texture {
    source: String,
    name: String,
    /// The pictures to embed; `None` for a canvas, which a model binds by name at run time.
    image: Option<TextureImage>,
}
/// The embeddable pictures of a texture entry.
struct TextureImage {
    sampler: Value,
    asset: String,
    pictures: Value,
    directory: PathBuf,
}
fn textures(
    published: &Path,
    documents: &[super::textures::ConvertedTextureDocument],
) -> CoreResult<Vec<Texture>> {
    let mut out = Vec::new();
    for source_document in documents {
        let source = &source_document.source_alias;
        let document = &source_document.document_path;
        let doc = &source_document.document;
        for entry in doc["entries"]
            .as_array()
            .ok_or_else(|| fail("texture entries missing"))?
        {
            if let Some(level) = entry["pictures"][0]["levels"][0]
                .as_str()
                .or_else(|| entry["variants"][0]["levels"][0].as_str())
            {
                out.push(Texture {
                    source: source.clone(),
                    name: entry["name"].as_str().unwrap_or("").into(),
                    image: Some(TextureImage {
                        sampler: entry["pictures"][0]["sampler"].clone(),
                        asset: png_identity(&Path::new(&document).parent().unwrap().join(level)),
                        pictures: if entry["pictures"].is_array() {
                            entry["pictures"].clone()
                        } else {
                            json!([{"levels":entry["variants"][0]["levels"]}])
                        },
                        directory: published
                            .join("data")
                            .join(Path::new(&document).parent().unwrap()),
                    }),
                });
            } else if entry["pictures"][0].get("canvas").is_some() {
                // A canvas claims its name for nearby models so a farther document's same-named
                // picture, and that picture's animation, are not embedded in its place.
                out.push(Texture {
                    source: source.clone(),
                    name: entry["name"].as_str().unwrap_or("").into(),
                    image: None,
                });
            }
        }
    }
    Ok(out)
}
fn resolve_texture<'a>(
    all: &'a [Texture],
    alias: &str,
    texture: &str,
) -> CoreResult<Option<&'a Texture>> {
    if texture.is_empty() {
        return Ok(None);
    }
    let candidates: Vec<_> = all
        .iter()
        .filter(|t| t.name.eq_ignore_ascii_case(texture))
        .collect();
    let mut best = None;
    let mut score = 0;
    for t in candidates {
        let common = t
            .source
            .bytes()
            .zip(parent(alias).bytes())
            .take_while(|(a, b)| a == b)
            .count();
        if best.is_none() || common > score {
            best = Some(t);
            score = common
        }
    }
    Ok(best)
}
fn material(
    gltf: &mut Gltf,
    b: &[u8],
    at: usize,
    alias: &str,
    all: &[Texture],
    used: &mut BTreeSet<String>,
) -> CoreResult<Value> {
    let diffuse = floats(b, at, 4)?;
    let ambient = floats(b, at + 16, 4)?;
    let specular = floats(b, at + 32, 4)?;
    let power = floats(b, at + 48, 1)?[0];
    let texture = name(bytes(b, at + 52, 32)?);
    let mut pbr = json!({"baseColorFactor":diffuse.iter().map(|v|v.clamp(0.,1.)).collect::<Vec<_>>(),"metallicFactor":0,"roughnessFactor":1});
    let selected_texture =
        resolve_texture(all, alias, &texture)?.and_then(|t| Some((t, t.image.as_ref()?)));
    let mut embedded_pictures = Value::Null;
    if let Some((t, image_entry)) = selected_texture {
        used.insert(t.source.clone());
        let images = gltf
            .doc
            .as_object_mut()
            .unwrap()
            .entry("images")
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .unwrap();
        let image_name = format!("{}/{}", t.source, t.name);
        let image = if let Some(id) = images.iter().position(|i| i["name"] == image_name) {
            id
        } else {
            let id = images.len();
            images.push(
                json!({"name":image_name,"extras":{"chronicle_two":{"png":image_entry.asset}}}),
            );
            id
        };
        let textures = gltf
            .doc
            .as_object_mut()
            .unwrap()
            .entry("textures")
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .unwrap();
        let id = textures.len();
        let samplers = gltf
            .doc
            .as_object_mut()
            .unwrap()
            .entry("samplers")
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .unwrap();
        let sampler = samplers.len();
        samplers.push(json!({"wrapS":if image_entry.sampler["wrap_u"]=="clamp"{33071}else{10497},"wrapT":if image_entry.sampler["wrap_v"]=="clamp"{33071}else{10497}}));
        gltf.doc["textures"]
            .as_array_mut()
            .unwrap()
            .push(json!({"source":image,"sampler":sampler}));
        pbr["baseColorTexture"] = json!({"index":id});
        let mut pictures = image_entry.pictures.clone();
        for (pi, picture) in pictures.as_array_mut().unwrap().iter_mut().enumerate() {
            if let Some(levels) = picture.get_mut("levels").and_then(Value::as_array_mut) {
                for (li, level) in levels.iter_mut().enumerate() {
                    let reference = level
                        .as_str()
                        .ok_or_else(|| fail("invalid model texture mip"))?;
                    let image = if pi == 0 && li == 0 {
                        image
                    } else {
                        add_image(
                            gltf,
                            &image_entry.directory.join(reference),
                            &format!("{}:{reference}", t.source),
                        )?
                    };
                    *level = json!(image);
                }
            }
            if let Some(variants) = picture.get_mut("variants").and_then(Value::as_array_mut) {
                for variant in variants {
                    for level in variant["levels"]
                        .as_array_mut()
                        .ok_or_else(|| fail("invalid model texture variant levels"))?
                    {
                        let reference = level
                            .as_str()
                            .ok_or_else(|| fail("invalid model texture variant mip"))?;
                        *level = json!(add_image(
                            gltf,
                            &image_entry.directory.join(reference),
                            &format!("{}:{reference}", t.source)
                        )?);
                    }
                }
            }
        }
        embedded_pictures = pictures;
    }
    gltf.doc["materials"].as_array_mut().unwrap().push(
        json!({"name":texture,"pbrMetallicRoughness":pbr,"doubleSided":true,"alphaMode":"BLEND"}),
    );
    Ok(
        json!({"texture":texture,"texture_source":selected_texture.map(|(t,_)|&t.source),"texture_path":selected_texture.map(|(_,i)|&i.asset),"texture_images":embedded_pictures,"diffuse":diffuse,"ambient":ambient,"specular":specular,"power":power,"flags":u32le(b,at+84)?,"extra":floats(b,at+88,2)?}),
    )
}
/// Adds a named image backed by a standalone PNG, reusing an existing image of that name.
fn add_image(gltf: &mut Gltf, path: &Path, name: &str) -> CoreResult<usize> {
    let images = gltf
        .doc
        .as_object_mut()
        .unwrap()
        .entry("images")
        .or_insert_with(|| json!([]))
        .as_array_mut()
        .unwrap();
    if let Some(id) = images.iter().position(|v| v["name"] == name) {
        return Ok(id);
    }
    let id = images.len();
    images.push(json!({"name":name,"extras":{"chronicle_two":{"png":png_identity(path)}}}));
    Ok(id)
}
/// Percent-encodes one URI path segment, retaining only unreserved characters.
fn encode_uri_segment(segment: &str) -> String {
    let mut out = String::with_capacity(segment.len());
    for byte in segment.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~') {
            out.push(char::from(byte));
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}
/// Builds the relative URI from a data document to another data file.
fn relative_uri(document: &str, target: &str) -> String {
    let mut base: Vec<&str> = document.split('/').collect();
    base.pop();
    let target: Vec<&str> = target.split('/').collect();
    let common = base
        .iter()
        .zip(&target[..target.len() - 1])
        .take_while(|(a, b)| a == b)
        .count();
    let mut parts = vec!["..".to_owned(); base.len() - common];
    parts.extend(target[common..].iter().map(|part| encode_uri_segment(part)));
    parts.join("/")
}
/// Names the binary buffer file published beside a glTF document.
fn buffer_path(document: &str) -> String {
    format!("{document}.bin")
}
/// Reads every buffer of a published glTF, embedded or stored in a data file.
fn load_buffers(port: &Path, document: &str, doc: &Value) -> CoreResult<Vec<Vec<u8>>> {
    let mut buffers = Vec::new();
    for buffer in doc["buffers"].as_array().into_iter().flatten() {
        let uri = buffer["uri"]
            .as_str()
            .ok_or_else(|| fail(format!("glTF buffer URI missing: {document}")))?;
        buffers.push(if uri.starts_with("data:") {
            decode_uri(uri, "data:application/octet-stream;base64,")?
        } else {
            fs::read(port.join(resolve_uri(document, uri)?))
                .map_err(|e| fail(format!("missing glTF buffer {uri} of {document}: {e}")))?
        });
    }
    Ok(buffers)
}
/// Stores a glTF's buffers as one `<document>.bin` file beside it and points the document at it.
///
/// Every buffer view keeps its bytes; views are rebased onto the merged, 4-byte aligned buffer.
/// Returns the data path of the buffer file, or `None` when the document has no buffers.
fn externalize_buffers(
    port: &Path,
    document: &str,
    gltf: &mut Value,
) -> CoreResult<Option<String>> {
    let buffers = load_buffers(port, document, gltf)?;
    if buffers.is_empty() {
        return Ok(None);
    }
    let mut merged = Vec::new();
    let mut starts = Vec::with_capacity(buffers.len());
    for buffer in &buffers {
        while !merged.len().is_multiple_of(4) {
            merged.push(0);
        }
        starts.push(merged.len());
        merged.extend_from_slice(buffer);
    }
    for view in gltf["bufferViews"].as_array_mut().into_iter().flatten() {
        let buffer = index(&view["buffer"], starts.len(), "buffer")?;
        let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize + starts[buffer];
        view["buffer"] = json!(0);
        if offset == 0 {
            view.as_object_mut().unwrap().remove("byteOffset");
        } else {
            view["byteOffset"] = json!(offset);
        }
    }
    let path = buffer_path(document);
    let target = port.join(&path);
    fs::create_dir_all(
        target
            .parent()
            .ok_or_else(|| fail("missing buffer parent"))?,
    )?;
    fs::write(&target, &merged)?;
    gltf["buffers"] = json!([{"byteLength":merged.len(),"uri":relative_uri(document, &path)}]);
    Ok(Some(path))
}

/// Externalizes generated buffers without reading or writing staged asset files.
fn externalize_buffers_memory(document: &str, gltf: &mut Value) -> CoreResult<Option<Vec<u8>>> {
    externalize_buffers_memory_with_existing(document, gltf, None)
}

fn externalize_buffers_memory_with_existing(
    document: &str,
    gltf: &mut Value,
    existing: Option<&[u8]>,
) -> CoreResult<Option<Vec<u8>>> {
    let mut buffers = Vec::new();
    for entry in gltf["buffers"].as_array().into_iter().flatten() {
        let uri = entry["uri"]
            .as_str()
            .ok_or_else(|| fail("generated glTF buffer URI missing"))?;
        if uri.starts_with("data:") {
            buffers.push(decode_uri(uri, "data:application/octet-stream;base64,")?);
        } else if uri == relative_uri(document, &buffer_path(document)) {
            buffers.push(
                existing
                    .ok_or_else(|| fail(format!("missing model buffer for {document}")))?
                    .to_vec(),
            );
        } else {
            return Err(fail(format!("unexpected generated glTF buffer URI: {uri}")));
        }
    }
    if buffers.is_empty() {
        return Ok(None);
    }
    let mut merged = Vec::new();
    let mut starts = Vec::with_capacity(buffers.len());
    for buffer in buffers {
        while !merged.len().is_multiple_of(4) {
            merged.push(0);
        }
        starts.push(merged.len());
        merged.extend_from_slice(&buffer);
    }
    for view in gltf["bufferViews"].as_array_mut().into_iter().flatten() {
        let buffer = index(&view["buffer"], starts.len(), "buffer")?;
        let offset = view["byteOffset"].as_u64().unwrap_or(0) as usize + starts[buffer];
        view["buffer"] = json!(0);
        if offset == 0 {
            view.as_object_mut().unwrap().remove("byteOffset");
        } else {
            view["byteOffset"] = json!(offset);
        }
    }
    gltf["buffers"] =
        json!([{"byteLength":merged.len(),"uri":relative_uri(document, &buffer_path(document))}]);
    Ok(Some(merged))
}
/// Points every glTF image at its standalone PNG relative to the glTF file.
fn link_images(
    published: &Path,
    path: &str,
    gltf: &mut Value,
    report: &mut ConversionReport,
    planned_pngs: Option<&BTreeSet<String>>,
) -> CoreResult<()> {
    let port = published.join("data");
    let document = path.replace('\\', "/");
    for image in gltf
        .get_mut("images")
        .and_then(Value::as_array_mut)
        .into_iter()
        .flatten()
    {
        let png = image["extras"]["chronicle_two"]["png"]
            .as_str()
            .ok_or_else(|| fail(format!("model image has no standalone PNG: {document}")))?
            .to_owned();
        if !planned_pngs.map_or_else(|| port.join(&png).is_file(), |names| names.contains(&png)) {
            return Err(fail(format!("missing model image {png}: {document}")));
        }
        image["uri"] = json!(relative_uri(&document, &png));
        report.referenced_pngs.insert(png);
    }
    Ok(())
}
fn png_identity(path: &Path) -> String {
    let normalized = path.to_string_lossy().replace('\\', "/");
    let relative = normalized
        .rsplit_once("/data/")
        .map_or(normalized.clone(), |(_, relative)| relative.to_owned());
    let mut parts = Vec::new();
    for part in relative.split('/') {
        match part {
            "." | "" => {}
            ".." => {
                parts.pop();
            }
            _ => parts.push(part),
        }
    }
    parts.join("/")
}
fn prune_metadata(value: &mut Value) {
    match value {
        Value::Object(object) => {
            for key in ["channels", "motion_channels"] {
                if let Some(channels) = object.get_mut(key).and_then(Value::as_array_mut) {
                    channels.retain(|channel| {
                        channel["keys"]
                            .as_array()
                            .is_some_and(|keys| !keys.is_empty())
                    });
                }
            }
            for key in [
                "source",
                "alias",
                "reserved_u32",
                "face_reserved_u32",
                "local_matrix",
                "parent",
                "frames_per_second",
            ] {
                object.remove(key);
            }
            if object.contains_key("diffuse") && object.contains_key("ambient") {
                object.retain(|key, _| matches!(key.as_str(), "diffuse" | "ambient" | "texture"));
            }
            if let Some(streams) = object.remove("streams") {
                let mut overrides = serde_json::Map::new();
                for (stream, map, width, w) in [
                    ("positions", "source_vertices", 3, 1.),
                    ("normals", "source_normals", 3, 0.),
                    ("uvs", "source_uvs", 2, 0.),
                    ("colours", "source_colours", 4, 0.),
                ] {
                    let used: BTreeSet<usize> = object
                        .get("primitives")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .flat_map(|p| p[map].as_array().into_iter().flatten())
                        .filter_map(|v| v.as_u64().map(|v| v as usize))
                        .collect();
                    let mut retained = Vec::new();
                    for (index, vector) in
                        streams[stream].as_array().into_iter().flatten().enumerate()
                    {
                        let original: Vec<f32> = vector
                            .as_array()
                            .unwrap()
                            .iter()
                            .map(|v| v.as_f64().unwrap() as f32)
                            .collect();
                        let length = original[..3].iter().map(|v| v * v).sum::<f32>().sqrt();
                        // `component` also picks the default past `width`, so this stays a range loop.
                        #[allow(clippy::needless_range_loop)]
                        for component in 0..4 {
                            let standard = if !used.contains(&index) {
                                0.
                            } else if component >= width {
                                if component == 3 { w } else { 0. }
                            } else if stream == "normals" {
                                if length > 1e-20 {
                                    original[component] / length
                                } else if component == 1 {
                                    1.
                                } else {
                                    0.
                                }
                            } else if stream == "colours" {
                                original[component].clamp(0., 1.)
                            } else {
                                original[component]
                            };
                            if original[component].to_bits() != standard.to_bits() {
                                retained.push(json!([index, component, original[component]]));
                            }
                        }
                    }
                    if !retained.is_empty() {
                        overrides.insert(stream.into(), json!(retained));
                    }
                }
                if !overrides.is_empty() {
                    object.insert("stream_overrides".into(), Value::Object(overrides));
                }
            }
            if object.contains_key("id") && object.contains_key("mdt") {
                object.remove("name");
            }
            for child in object.values_mut() {
                prune_metadata(child);
            }
        }
        Value::Array(values) => {
            for child in values {
                prune_metadata(child)
            }
        }
        _ => {}
    }
}
#[cfg(test)]
fn publish_pair(
    published: &Path,
    path: &str,
    gltf: &mut Value,
    metadata: &mut Value,
    report: &mut ConversionReport,
) -> CoreResult<()> {
    publish_pair_impl(published, path, gltf, metadata, report, None)
}

struct StreamPair {
    gltf: Value,
    metadata: Value,
    buffer: Option<Vec<u8>>,
}

struct PackedPair {
    gltf: Vec<u8>,
    metadata: Vec<u8>,
    buffer: Option<Vec<u8>>,
}

impl StreamPair {
    fn compact(self) -> CoreResult<PackedPair> {
        Ok(PackedPair {
            gltf: serde_json::to_vec(&self.gltf)?,
            metadata: serde_json::to_vec(&self.metadata)?,
            buffer: self.buffer,
        })
    }
}

struct StreamPairs<'a> {
    pairs: &'a mut BTreeMap<String, StreamPair>,
    png_names: &'a BTreeSet<String>,
    gameplay: Option<&'a mut BTreeMap<String, Value>>,
    preview: bool,
}

fn publish_pair_impl(
    published: &Path,
    path: &str,
    gltf: &mut Value,
    metadata: &mut Value,
    report: &mut ConversionReport,
    mut stream: Option<&mut StreamPairs<'_>>,
) -> CoreResult<()> {
    if let Some(output) = stream.as_mut()
        && output.preview
    {
        let document = path.replace('\\', "/");
        if output
            .pairs
            .insert(
                document.clone(),
                StreamPair {
                    gltf: Value::Null,
                    metadata: Value::Null,
                    buffer: None,
                },
            )
            .is_some()
        {
            return Err(fail(format!("duplicate model output: {document}")));
        }
        return Ok(());
    }
    if metadata["kind"] == "character" {
        for model in metadata["models"].as_array_mut().unwrap() {
            model.as_object_mut().unwrap().remove("name");
            for key in ["motion_channels", "bind_poses", "weights", "schema_version"] {
                model.as_object_mut().unwrap().remove(key);
            }
        }
        for resources in metadata["resources"].as_object_mut().unwrap().values_mut() {
            for resource in resources.as_array_mut().unwrap() {
                resource.as_object_mut().unwrap().remove("name");
            }
        }
    } else if gltf["animations"].as_array().is_some_and(|a| !a.is_empty()) {
        metadata["motion_channels"]
            .as_array_mut()
            .unwrap()
            .retain(|c| c["type"].as_i64().unwrap() > 2);
    }
    fn minimal_model(model: &mut Value) {
        if let Some(frames) = model.get_mut("frames").and_then(Value::as_array_mut) {
            for frame in frames {
                frame.as_object_mut().unwrap().remove("id");
                frame.as_object_mut().unwrap().remove("name");
            }
        }
        if let Some(meshes) = model.get_mut("meshes").and_then(Value::as_array_mut) {
            for mesh in meshes {
                mesh.as_object_mut().unwrap().remove("frame");
            }
        }
    }
    if let Some(models) = metadata.get_mut("models").and_then(Value::as_array_mut) {
        for model in models {
            minimal_model(model);
        }
    } else {
        minimal_model(metadata);
    }
    link_images(
        published,
        path,
        gltf,
        report,
        stream.as_ref().map(|output| output.png_names),
    )?;
    let mut images = Vec::new();
    if let Some(values) = gltf.get_mut("images").and_then(Value::as_array_mut) {
        for (image, value) in values.iter_mut().enumerate() {
            if let Some(png) = value["extras"]["chronicle_two"]["png"].as_str() {
                images.push(json!({"image":image,"png":png}));
            }
            value.as_object_mut().unwrap().remove("extras");
        }
    }
    if !images.is_empty() {
        metadata["images"] = json!(images);
    }
    if let Some(meshes) = gltf.get_mut("meshes").and_then(Value::as_array_mut) {
        for mesh in meshes {
            mesh.as_object_mut().unwrap().remove("extras");
        }
    }
    gltf.as_object_mut().unwrap().remove("extras");
    prune_metadata(metadata);
    let document = path.replace('\\', "/");
    if let Some(output) = stream.as_mut() {
        if metadata["kind"] == "character" {
            if let Some(gameplay) = output.gameplay.as_deref_mut() {
                let mut external = json!({"schema_version":1,"kind":"character_gameplay",
                    "profiles":metadata["profiles"],"game_names":metadata["game_names"]});
                if !metadata["default_profile"].is_null() {
                    external["default_profile"] = metadata["default_profile"].clone();
                }
                let cloth: Vec<Value> = metadata["resources"]["cloths"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .enumerate()
                    .filter_map(|(index, row)| {
                        row.get("commands")
                            .map(|commands| json!({"index":index,"commands":commands}))
                    })
                    .collect();
                if !cloth.is_empty() {
                    external["cloth_commands"] = json!(cloth);
                }
                let path = Path::new(path)
                    .with_extension("character.json")
                    .to_string_lossy()
                    .replace('\\', "/");
                if gameplay.insert(path.clone(), external).is_some() {
                    return Err(fail(format!("duplicate gameplay document: {path}")));
                }
            }
            metadata.as_object_mut().unwrap().remove("profiles");
            metadata.as_object_mut().unwrap().remove("game_names");
            metadata.as_object_mut().unwrap().remove("default_profile");
            for cloth in metadata["resources"]["cloths"]
                .as_array_mut()
                .into_iter()
                .flatten()
            {
                cloth.as_object_mut().unwrap().remove("commands");
            }
        }
        let buffer = externalize_buffers_memory(&document, gltf)?;
        let sidecar = Path::new(path)
            .with_extension("game.json")
            .to_string_lossy()
            .replace('\\', "/");
        if output
            .pairs
            .insert(
                document.clone(),
                StreamPair {
                    gltf: gltf.clone(),
                    metadata: metadata.clone(),
                    buffer,
                },
            )
            .is_some()
        {
            return Err(fail(format!("duplicate model output: {document}")));
        }
        report.files.push(document);
        report.files.push(sidecar);
        return Ok(());
    }
    if let Some(buffer) = externalize_buffers(&published.join("data"), &document, gltf)? {
        report.files.push(buffer);
    }
    save(&published.join("data").join(path), gltf)?;
    let sidecar = Path::new(path).with_extension("game.json");
    save(&published.join("data").join(&sidecar), metadata)?;
    report.files.push(path.into());
    report
        .files
        .push(sidecar.to_string_lossy().replace('\\', "/"));
    Ok(())
}
fn identity() -> Vec<f32> {
    vec![
        1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.,
    ]
}
fn multiply(a: &[f32], b: &[f32]) -> Vec<f32> {
    let mut out = vec![0.; 16];
    for col in 0..4 {
        for row in 0..4 {
            for k in 0..4 {
                out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    out
}
fn inverse(m: &[f32]) -> CoreResult<Vec<f32>> {
    let mut a = [[0.; 8]; 4];
    for r in 0..4 {
        for c in 0..4 {
            a[r][c] = m[c * 4 + r]
        }
        a[r][r + 4] = 1.;
    }
    for c in 0..4 {
        let pivot = (c..4)
            .max_by(|&x, &y| a[x][c].abs().total_cmp(&a[y][c].abs()))
            .unwrap();
        a.swap(c, pivot);
        let d = a[c][c];
        if d.abs() < 1e-20 {
            return Err(fail("singular bind matrix"));
        }
        for v in &mut a[c] {
            *v /= d
        }
        for r in 0..4 {
            if r != c {
                let f = a[r][c];
                let pivot = a[c];
                for (v, p) in a[r].iter_mut().zip(pivot) {
                    *v -= f * p
                }
            }
        }
    }
    Ok((0..16).map(|i| a[i % 4][4 + i / 4]).collect())
}
fn trs(m: &[f32]) -> Value {
    let mut scale: Vec<f32> = (0..3)
        .map(|c| (m[c * 4].powi(2) + m[c * 4 + 1].powi(2) + m[c * 4 + 2].powi(2)).sqrt())
        .collect();
    let determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
        + m[8] * (m[1] * m[6] - m[5] * m[2]);
    if determinant < 0. {
        scale[0] = -scale[0];
    }
    let r: Vec<f32> = (0..9)
        .map(|i| {
            m[(i / 3) * 4 + i % 3]
                / if scale[i / 3].abs() > 1e-20 {
                    scale[i / 3]
                } else {
                    1.
                }
        })
        .collect();
    let trace = r[0] + r[4] + r[8];
    let q = if trace > 0. {
        let s = (trace + 1.).sqrt() * 2.;
        vec![
            (r[5] - r[7]) / s,
            (r[6] - r[2]) / s,
            (r[1] - r[3]) / s,
            s / 4.,
        ]
    } else {
        let i = (0..3)
            .max_by(|&a, &b| r[a * 3 + a].total_cmp(&r[b * 3 + b]))
            .unwrap();
        let j = (i + 1) % 3;
        let k = (i + 2) % 3;
        let s = (1. + r[i * 3 + i] - r[j * 3 + j] - r[k * 3 + k])
            .max(0.)
            .sqrt()
            * 2.;
        let mut v = vec![0.; 4];
        v[i] = s / 4.;
        v[j] = (r[i * 3 + j] + r[j * 3 + i]) / s;
        v[k] = (r[i * 3 + k] + r[k * 3 + i]) / s;
        v[3] = (r[j * 3 + k] - r[k * 3 + j]) / s;
        v
    };
    let norm = q.iter().map(|v| v * v).sum::<f32>().sqrt();
    let q = if norm > 1e-20 {
        q.iter().map(|v| v / norm).collect::<Vec<_>>()
    } else {
        vec![0., 0., 0., 1.]
    };
    json!({"translation":&m[12..15],"rotation":q,"scale":scale})
}

type Matrix3 = [[f32; 3]; 3];

/// Converts a standard rotation into a column-vector rotation matrix.
fn quaternion_matrix(q: &[f32]) -> Matrix3 {
    let [x, y, z, w] = [q[0], q[1], q[2], q[3]];
    [
        [
            1. - 2. * (y * y + z * z),
            2. * (x * y - w * z),
            2. * (x * z + w * y),
        ],
        [
            2. * (x * y + w * z),
            1. - 2. * (x * x + z * z),
            2. * (y * z - w * x),
        ],
        [
            2. * (x * z - w * y),
            2. * (y * z + w * x),
            1. - 2. * (x * x + y * y),
        ],
    ]
}
/// Reads the 3x3 basis of a column-major glTF matrix as rows and columns.
fn basis(m: &[f32]) -> Matrix3 {
    std::array::from_fn(|r| std::array::from_fn(|c| m[c * 4 + r]))
}
fn product(a: &Matrix3, b: &Matrix3) -> Matrix3 {
    std::array::from_fn(|r| std::array::from_fn(|c| (0..3).map(|k| a[r][k] * b[k][c]).sum()))
}
fn diagonal(s: &[f32]) -> Matrix3 {
    std::array::from_fn(|r| std::array::from_fn(|c| if r == c { s[r] } else { 0. }))
}
/// Largest element difference of two bases, relative to their largest element.
fn difference(a: &Matrix3, b: &Matrix3) -> f32 {
    let size = a.iter().flatten().fold(1f32, |m, v| m.max(v.abs()));
    (0..9)
        .map(|i| (a[i / 3][i % 3] - b[i / 3][i % 3]).abs())
        .fold(0., f32::max)
        / size
}
/// Measures how far a basis is from a rotation followed by an axis scale.
fn shear(m: &Matrix3) -> f32 {
    let g: Matrix3 =
        std::array::from_fn(|r| std::array::from_fn(|c| (0..3).map(|k| m[k][r] * m[k][c]).sum()));
    let mut worst = 0f32;
    for r in 0..3 {
        for c in 0..3 {
            let scale = (g[r][r] * g[c][c]).sqrt();
            if r != c && scale > 1e-12 {
                worst = worst.max(g[r][c].abs() / scale);
            }
        }
    }
    worst
}
/// Splits an unsheared basis into a standard rotation and axis scale.
fn rotation_scale(m: &Matrix3) -> ([f32; 4], [f32; 3]) {
    let mut column = [0f32; 16];
    for r in 0..3 {
        for c in 0..3 {
            column[c * 4 + r] = m[r][c];
        }
    }
    column[15] = 1.;
    let decomposed = trs(&column);
    let q = decomposed["rotation"].as_array().unwrap();
    let s = decomposed["scale"].as_array().unwrap();
    (
        std::array::from_fn(|i| q[i].as_f64().unwrap() as f32),
        std::array::from_fn(|i| s[i].as_f64().unwrap() as f32),
    )
}

/// How one animated retail frame is expressed in standard glTF.
enum FramePlan {
    /// Retail keys are standard rotation and scale keys.
    Plain,
    /// The retail pose has no shear but its rotation and scale must be rebaked.
    Baked,
    /// The retail pose has shear, so scale and rotation live on separate nodes.
    Helper(String),
}

/// Retail pose basis of a frame: scale applied after the rotation key or the authored basis.
fn retail_basis(
    authored: &Matrix3,
    rotation: Option<&[Value]>,
    scale: &[Value],
    frame: f32,
) -> Matrix3 {
    let b = rotation.map_or(*authored, |keys| {
        quaternion_matrix(&super::actions::sample_transform(keys, true, frame))
    });
    product(
        &diagonal(&super::actions::sample_transform(scale, false, frame)),
        &b,
    )
}

/// Chooses the standard representation of one animated frame.
fn plan_frame(matrix: &[f32], rotation: Option<&[Value]>, scale: Option<&[Value]>) -> FramePlan {
    let Some(scale) = scale else {
        return FramePlan::Plain;
    };
    let authored = basis(matrix);
    let (authored_rotation, _) = rotation_scale(&authored);
    let rest = quaternion_matrix(&authored_rotation);
    let (first, last) = scale
        .iter()
        .chain(rotation.into_iter().flatten())
        .filter_map(|k| k["frame"].as_u64())
        .fold((u64::MAX, 0), |(a, b), f| (a.min(f), b.max(f)));
    let mut plain = true;
    let mut worst = (0f32, first);
    for frame in first..=last {
        let retail = retail_basis(&authored, rotation, scale, frame as f32);
        let s = super::actions::sample_transform(scale, false, frame as f32);
        let r = rotation.map_or(rest, |keys| {
            quaternion_matrix(&super::actions::sample_transform(keys, true, frame as f32))
        });
        if difference(&retail, &product(&r, &diagonal(&s))) > 1e-5 {
            plain = false;
        }
        let amount = shear(&retail);
        if amount > worst.0 {
            worst = (amount, frame);
        }
    }
    // Shear below a thousandth of the scale is dropped by rebaking; the reported pose error covers it.
    if worst.0 > 1e-3 {
        return FramePlan::Helper(format!("shear {:.3} at source frame {}", worst.0, worst.1));
    }
    if plain {
        FramePlan::Plain
    } else {
        FramePlan::Baked
    }
}

/// Chooses a standard representation for every frame the motions animate.
///
/// A frame keeps a shear helper when any motion needs one, and is rebaked when any needs that.
fn prepare_transform_nodes(gltf: &mut Gltf, motions: &[&[Value]]) -> CoreResult<()> {
    let mut plans = BTreeMap::<usize, (FramePlan, bool)>::new();
    for channels in motions {
        for (frame, [rotation, scale, _]) in transform_keys(channels) {
            if frame >= gltf.doc["nodes"].as_array().unwrap().len() {
                return Err(fail("transform animation frame outside skeleton"));
            }
            let matrix: Vec<f32> = gltf.doc["nodes"][frame]
                .get("matrix")
                .and_then(Value::as_array)
                .map(|values| values.iter().map(|v| v.as_f64().unwrap() as f32).collect())
                .unwrap_or_else(identity);
            let plan = plan_frame(&matrix, rotation, scale);
            let entry = plans.entry(frame).or_insert((FramePlan::Plain, false));
            entry.1 |= rotation.is_some();
            entry.0 = match (std::mem::replace(&mut entry.0, FramePlan::Plain), plan) {
                (FramePlan::Helper(reason), _) | (_, FramePlan::Helper(reason)) => {
                    FramePlan::Helper(reason)
                }
                (FramePlan::Baked, _) | (_, FramePlan::Baked) => FramePlan::Baked,
                _ => FramePlan::Plain,
            };
        }
    }
    for (frame, (plan, rotated)) in plans {
        let matrix: Vec<f32> = gltf.doc["nodes"][frame]
            .get("matrix")
            .and_then(Value::as_array)
            .map(|values| values.iter().map(|v| v.as_f64().unwrap() as f32).collect())
            .unwrap_or_else(identity);
        let reason = match plan {
            FramePlan::Plain => {
                gltf.stats.plain += 1;
                continue;
            }
            FramePlan::Baked => {
                gltf.stats.baked += 1;
                gltf.baked.insert(frame);
                continue;
            }
            FramePlan::Helper(reason) => reason,
        };
        let rotation = rotated.then_some(());
        let authored_scale = trs(&matrix)["scale"]
            .as_array()
            .unwrap()
            .iter()
            .any(|value| (value.as_f64().unwrap() - 1.).abs() > 1e-5);
        let helper = gltf.doc["nodes"].as_array().unwrap().len();
        let mut basis = matrix.clone();
        basis[12..15].fill(0.);
        let mut child = json!({"matrix":basis});
        let node = &mut gltf.doc["nodes"][frame];
        let name = node["name"].as_str().unwrap_or("").to_owned();
        gltf.stats
            .helpers
            .push(format!("{name} (frame {frame}): {reason}"));
        if !name.is_empty() {
            child["name"] = json!(format!("{name}.rotation"));
        }
        for key in ["mesh", "skin", "children", "weights"] {
            if let Some(value) = node.as_object_mut().unwrap().remove(key) {
                child[key] = value;
            }
        }
        node.as_object_mut().unwrap().remove("matrix");
        node["translation"] = json!(&matrix[12..15]);
        node["children"] = json!([helper]);
        gltf.doc["nodes"].as_array_mut().unwrap().push(child);
        gltf.rotation_nodes.insert(frame, helper);
        if rotation.is_some() && authored_scale {
            gltf.reset_rotation_scale.insert(frame);
        }
    }
    if let Some(skins) = gltf.doc.get_mut("skins").and_then(Value::as_array_mut) {
        for skin in skins {
            for joint in skin["joints"].as_array_mut().unwrap() {
                if let Some(helper) = joint
                    .as_u64()
                    .and_then(|value| gltf.rotation_nodes.get(&(value as usize)))
                {
                    *joint = json!(helper);
                }
            }
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn mesh(
    gltf: &mut Gltf,
    b: &[u8],
    frame: usize,
    alias: &str,
    all: &[Texture],
    used: &mut BTreeSet<String>,
    weight_channels: &[Value],
    world: &[Vec<f32>],
) -> CoreResult<Value> {
    mesh_extent(b)?;
    let counts = [
        u32le(b, 12)? as usize,
        u32le(b, 20)? as usize,
        u32le(b, 44)? as usize,
        u32le(b, 28)? as usize,
    ];
    let offsets = [
        u32le(b, 16)? as usize,
        u32le(b, 24)? as usize,
        u32le(b, 48)? as usize,
        u32le(b, 32)? as usize,
    ];
    let material_base = gltf.doc["materials"].as_array().unwrap().len();
    let mut materials = Vec::new();
    for i in 0..u32le(b, 52)? as usize {
        materials.push(material(
            gltf,
            b,
            u32le(b, 56)? as usize + i * 96,
            alias,
            all,
            used,
        )?)
    }
    let mut influences = vec![Vec::<(u16, f32)>::new(); counts[0]];
    let mut bones = Vec::new();
    for channel in weight_channels.iter().filter(|c| {
        c["frame"].as_u64() == Some(frame as u64)
            && c["keys"].as_array().is_some_and(|keys| !keys.is_empty())
    }) {
        let bone = channel["target"]
            .as_u64()
            .ok_or_else(|| fail("negative skin bone"))? as usize;
        if bone >= world.len() {
            return Err(fail("skin bone outside skeleton"));
        }
        if !bones.contains(&bone) {
            bones.push(bone)
        }
        let slot = bones.iter().position(|&v| v == bone).unwrap() as u16;
        for key in channel["keys"].as_array().unwrap() {
            let vertex = key["frame"].as_u64().unwrap() as usize;
            if vertex >= influences.len() {
                return Err(fail("skin vertex outside MDT"));
            }
            influences[vertex].push((slot, key["value"][0].as_f64().unwrap() as f32 / 100.));
        }
    }
    if !bones.is_empty() {
        let mut matrices = Vec::new();
        for &bone in &bones {
            matrices.extend(multiply(&inverse(&world[bone])?, &world[frame]))
        }
        let a = gltf.accessor(&matrices, 16, "MAT4", false);
        let skins = gltf
            .doc
            .as_object_mut()
            .unwrap()
            .entry("skins")
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .unwrap();
        let id = skins.len();
        skins.push(json!({"joints":bones,"inverseBindMatrices":a}));
        gltf.doc["nodes"][frame]["skin"] = json!(id);
    }
    let face_offset = u32le(b, 40)? as usize;
    let mut at = face_offset + u32le(b, face_offset + 4)? as usize;
    let primitive_count = u32le(b, face_offset + 8)? as usize;
    let mut primitives = Vec::new();
    let mut metadata = Vec::new();
    let influence_sets = influences
        .iter()
        .map(Vec::len)
        .max()
        .unwrap_or(0)
        .max(1)
        .div_ceil(4);
    for _ in 0..primitive_count {
        let kind = u32le(b, at)?;
        let corners = u32le(b, at + 4)? as usize;
        let mat = u32le(b, at + 8)? as usize;
        at += 12;
        let enabled = [true, kind & 0x200 == 0, kind & 0x10 == 0, kind & 0x100 != 0];
        let mut streams = [Vec::new(), Vec::new(), Vec::new(), Vec::new()];
        let mut ids = [Vec::new(), Vec::new(), Vec::new(), Vec::new()];
        let mut joints = vec![Vec::new(); influence_sets];
        let mut weights = vec![Vec::new(); influence_sets];
        for _ in 0..corners {
            for s in 0..4 {
                if enabled[s] {
                    let id = u32le(b, at)? as usize;
                    at += 4;
                    if id >= counts[s] {
                        return Err(fail(format!(
                            "MDT stream {s} index {id} outside {} ({alias})",
                            counts[s]
                        )));
                    }
                    ids[s].push(id);
                    let mut v = floats(
                        b,
                        offsets[s] + id * 16,
                        if s == 2 {
                            2
                        } else if s == 3 {
                            4
                        } else {
                            3
                        },
                    )?;
                    if s == 1 {
                        let length = v.iter().map(|x| x * x).sum::<f32>().sqrt();
                        if length > 1e-20 {
                            for x in &mut v {
                                *x /= length
                            }
                        } else {
                            v[1] = 1.;
                        }
                    }
                    if s == 3 {
                        for x in &mut v {
                            *x = x.clamp(0., 1.);
                        }
                    }
                    streams[s].extend(v);
                    if s == 0 && !bones.is_empty() {
                        let influences = &influences[id];
                        let total: f32 = influences.iter().map(|v| v.1).sum();
                        for group in 0..influence_sets {
                            for component in 0..4 {
                                let k = group * 4 + component;
                                joints[group].push(influences.get(k).map_or(0, |v| v.0));
                                weights[group].push(
                                    influences
                                        .get(k)
                                        .map_or(if total == 0. && k == 0 { 1. } else { 0. }, |v| {
                                            v.1 / if total > 0. { total } else { 1. }
                                        }),
                                );
                            }
                        }
                    }
                }
            }
        }
        if corners == 0 {
            continue;
        }
        let mut attributes = json!({});
        for s in 0..4 {
            if enabled[s] {
                let width = if s == 2 {
                    2
                } else if s == 3 {
                    4
                } else {
                    3
                };
                let accessor = gltf.accessor(
                    &streams[s],
                    width,
                    if width == 2 {
                        "VEC2"
                    } else if width == 4 {
                        "VEC4"
                    } else {
                        "VEC3"
                    },
                    s == 0,
                );
                attributes[["POSITION", "NORMAL", "TEXCOORD_0", "COLOR_0"][s]] = json!(accessor);
            }
        }
        if !bones.is_empty() {
            for group in 0..influence_sets {
                attributes[format!("JOINTS_{group}")] = json!(gltf.joints(&joints[group]));
                attributes[format!("WEIGHTS_{group}")] =
                    json!(gltf.accessor(&weights[group], 4, "VEC4", false));
            }
        }
        let mode = match kind & 7 {
            0 => 0,
            1 => 1,
            2 => 3,
            3 => 4,
            4 => 5,
            5 => 6,
            _ => return Err(fail(format!("unsupported MDT primitive {kind} in {alias}"))),
        };
        let mut primitive = json!({"attributes":attributes,"mode":mode});
        if mat < materials.len() {
            primitive["material"] = json!(material_base + mat)
        }
        primitives.push(primitive);
        metadata.push(json!({"type":kind,"material":mat,"source_vertices":ids[0],"source_normals":ids[1],"source_uvs":ids[2],"source_colours":ids[3]}));
    }
    let id = gltf.doc["meshes"].as_array().unwrap().len();
    if !primitives.is_empty() {
        gltf.doc["meshes"]
            .as_array_mut()
            .unwrap()
            .push(json!({"primitives":primitives}));
        gltf.doc["nodes"][frame]["mesh"] = json!(id);
    }
    let mut original_streams = Vec::new();
    for s in 0..4 {
        original_streams.push(
            (0..counts[s])
                .map(|i| floats(b, offsets[s] + i * 16, 4))
                .collect::<CoreResult<Vec<_>>>()?,
        );
    }
    Ok(
        json!({"frame":frame,"vertex_count":counts[0],"normal_count":counts[1],"uv_count":counts[2],"colour_count":counts[3],"primitives":metadata,"materials":materials,"reserved_u32":[u32le(b,8)?,u32le(b,60)?],"face_reserved_u32":[u32le(b,face_offset)?,u32le(b,face_offset+12)?],"streams":{"positions":original_streams[0],"normals":original_streams[1],"uvs":original_streams[2],"colours":original_streams[3]}}),
    )
}

fn prepare_morphs(gltf: &mut Gltf, meshes: &[Value], channels: &[Value]) -> CoreResult<()> {
    for mesh in meshes {
        let frame = mesh["frame"].as_u64().unwrap() as usize;
        let Some(mesh_id) = gltf.doc["nodes"][frame]["mesh"]
            .as_u64()
            .map(|v| v as usize)
        else {
            continue;
        };
        let vertices: BTreeSet<_> = channels
            .iter()
            .filter(|c| c["type"] == 12 && c["frame"].as_u64() == Some(frame as u64))
            .filter_map(|c| c["target"].as_u64())
            .collect();
        if vertices.is_empty() {
            continue;
        }
        let mut targets = Vec::new();
        for vertex in vertices {
            if vertex == 0 || vertex as usize > mesh["vertex_count"].as_u64().unwrap() as usize {
                return Err(fail("morph vertex outside MDT"));
            }
            for axis in 0..3 {
                targets.push((vertex as usize - 1, axis));
            }
        }
        for (primitive_index, primitive) in
            mesh["primitives"].as_array().unwrap().iter().enumerate()
        {
            let source = primitive["source_vertices"].as_array().unwrap();
            let mut morphs = Vec::new();
            for &(vertex, axis) in &targets {
                let indices: Vec<_> = source
                    .iter()
                    .enumerate()
                    .filter(|(_, v)| v.as_u64() == Some(vertex as u64))
                    .map(|(i, _)| i as u32)
                    .collect();
                let a = if indices.is_empty() {
                    gltf.accessor(&vec![0.; source.len() * 3], 3, "VEC3", true)
                } else {
                    gltf.sparse_axis(source.len(), &indices, axis)
                };
                morphs.push(json!({"POSITION":a}));
            }
            gltf.doc["meshes"][mesh_id]["primitives"][primitive_index]["targets"] = json!(morphs);
        }
        let target_metadata:Vec<_>=targets.iter().map(|&(vertex,axis)|json!({"vertex":vertex,"axis":axis,"base":mesh["streams"]["positions"][vertex][axis]})).collect();
        gltf.doc["meshes"][mesh_id]["weights"] = json!(vec![0.; targets.len()]);
        gltf.doc["meshes"][mesh_id]["extras"] = json!({"kazarov_morph_targets":target_metadata});
    }
    Ok(())
}
fn compatible_motion(channels: &[Value], frames: usize) -> bool {
    channels
        .iter()
        .filter(|c| matches!(c["type"].as_i64(), Some(0..=2 | 12)))
        .all(|c| c["frame"].as_u64().is_some_and(|n| n < (frames as u64)))
}
fn sample_key(keys: &[Value], time: u64, axis: usize) -> f32 {
    let next = keys.partition_point(|k| k["frame"].as_u64().unwrap() <= time);
    let left = next.saturating_sub(1);
    let right = next.min(keys.len() - 1);
    let a = &keys[left];
    let b = &keys[right];
    let start = a["frame"].as_u64().unwrap();
    let end = b["frame"].as_u64().unwrap();
    let t = if end == start {
        0.
    } else {
        (time.saturating_sub(start) as f32 / (end - start) as f32).clamp(0., 1.)
    };
    let x = a["value"][axis].as_f64().unwrap() as f32;
    let y = b["value"][axis].as_f64().unwrap() as f32;
    x + (y - x) * t
}
/// Retail key lists of each animated frame and transform type; later lists replace earlier ones.
fn transform_keys(channels: &[Value]) -> BTreeMap<usize, [Option<&[Value]>; 3]> {
    let mut animated = BTreeMap::<usize, [Option<&[Value]>; 3]>::new();
    for channel in channels {
        let (Some(frame), Some(kind)) = (
            channel["frame"].as_u64().map(|value| value as usize),
            channel["type"].as_i64().filter(|k| (0..=2).contains(k)),
        ) else {
            continue;
        };
        let keys = channel["keys"].as_array().map_or(&[][..], Vec::as_slice);
        let entry = animated.entry(frame).or_default();
        entry[kind as usize] = (!keys.is_empty()).then_some(keys);
    }
    animated
}

/// Replaces a node's authored matrix by its translation, rotation and scale.
///
/// The matrix is kept in `authored`, since every later clip rebakes from it.
fn decompose_node(gltf: &mut Gltf, node: usize) {
    if let Some(matrix) = gltf.doc["nodes"][node]["matrix"].as_array() {
        let matrix: Vec<f32> = matrix.iter().map(|v| v.as_f64().unwrap() as f32).collect();
        let decomposed = trs(&matrix);
        let object = gltf.doc["nodes"][node].as_object_mut().unwrap();
        object.remove("matrix");
        for (key, value) in decomposed.as_object().unwrap() {
            object.insert(key.clone(), value.clone());
        }
        gltf.authored.insert(node, matrix);
    }
}

/// Adds one LINEAR channel with local key times in game frames.
fn add_channel(
    gltf: &mut Gltf,
    samplers: &mut Vec<Value>,
    targets: &mut Vec<Value>,
    node: usize,
    path: &str,
    keys: &[(u64, Vec<f32>)],
) {
    // A channel that never changes within the action needs one key.
    let keys = if keys.iter().all(|(_, v)| *v == keys[0].1) {
        &keys[..1]
    } else {
        keys
    };
    let width = keys[0].1.len();
    let times: Vec<f32> = keys.iter().map(|(f, _)| *f as f32 / 30.).collect();
    let values: Vec<f32> = keys.iter().flat_map(|(_, v)| v.iter().copied()).collect();
    let input = gltf.animation_accessor(&times, 1, true);
    let output = gltf.animation_accessor(&values, width, false);
    samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
    targets.push(json!({"sampler":samplers.len()-1,"target":{"node":node,"path":path}}));
}

/// Keys of one channel: frames local to an action and their values.
type LocalKeys = Vec<(u64, Vec<f32>)>;

/// Rebakes a frame's retail scale-after-rotation pose as standard rotation and scale keys.
fn baked_keys(
    gltf: &mut Gltf,
    matrix: &[f32],
    rotation: Option<&[Value]>,
    scale: &[Value],
    start: u64,
    end: u64,
) -> (LocalKeys, LocalKeys) {
    let authored = basis(matrix);
    let mut rotations = Vec::new();
    let mut scales = Vec::new();
    let mut previous: Option<[f32; 4]> = None;
    for frame in start..=end {
        let retail = retail_basis(&authored, rotation, scale, frame as f32);
        let (mut q, s) = rotation_scale(&retail);
        gltf.stats.baked_error = gltf.stats.baked_error.max(difference(
            &retail,
            &product(&quaternion_matrix(&q), &diagonal(&s)),
        ));
        if let Some(p) = previous {
            if (0..4).map(|i| p[i] * q[i]).sum::<f32>() < 0. {
                q = q.map(|v| -v);
            }
            // Measures the pose between rebaked keys against the retail pose there.
            let half = frame as f32 - 0.5;
            let retail = retail_basis(&authored, rotation, scale, half);
            let ps: &Vec<f32> = &scales
                .last()
                .map(|(_, v): &(u64, Vec<f32>)| v.clone())
                .unwrap();
            let blended = super::actions::retail_slerp(p, q, 0.5);
            let norm = blended.iter().map(|v| v * v).sum::<f32>().sqrt();
            let mid: Vec<f32> = (0..3).map(|i| (ps[i] + s[i]) * 0.5).collect();
            let standard = product(
                &quaternion_matrix(&blended.map(|v| v / norm)),
                &diagonal(&mid),
            );
            gltf.stats.baked_error = gltf.stats.baked_error.max(difference(&retail, &standard));
        }
        previous = Some(q);
        rotations.push((frame - start, q.to_vec()));
        scales.push((frame - start, s.to_vec()));
    }
    (rotations, scales)
}

/// Builds one named action clip from the retail keys of a source interval.
///
/// Key times are local to the interval; retail-sampled keys are added at its boundaries.
fn animation_clip(
    gltf: &mut Gltf,
    name: &str,
    channels: &[Value],
    (start, end): (u64, u64),
    node_offset: usize,
    morphs: bool,
) -> CoreResult<Option<usize>> {
    let mut samplers = Vec::new();
    let mut targets = Vec::new();
    let node_count = gltf.doc["nodes"].as_array().unwrap().len();
    for (frame, [rotation, scale, translation]) in transform_keys(channels) {
        let node = node_offset + frame;
        if node >= node_count {
            return Err(fail(format!("motion frame {frame} outside model {name}")));
        }
        let helper = gltf.rotation_nodes.get(&node).copied();
        decompose_node(gltf, node);
        // The first clip decomposes the node; a frame without rotation keys keeps this matrix in every clip.
        let matrix = gltf.authored.get(&node).cloned().unwrap_or_else(identity);
        if let Some(helper) = helper {
            decompose_node(gltf, helper);
        }
        let mut boundary = gltf.stats.boundary_keys;
        if let Some(keys) = translation {
            let sliced = super::actions::slice_keys(keys, false, start, end, &mut boundary);
            add_channel(
                gltf,
                &mut samplers,
                &mut targets,
                node,
                "translation",
                &sliced,
            );
        }
        if let (Some(scale), true) = (scale, gltf.baked.contains(&node)) {
            let (rotations, scales) = baked_keys(gltf, &matrix, rotation, scale, start, end);
            add_channel(
                gltf,
                &mut samplers,
                &mut targets,
                node,
                "rotation",
                &rotations,
            );
            add_channel(gltf, &mut samplers, &mut targets, node, "scale", &scales);
            gltf.stats.boundary_keys = boundary;
            continue;
        }
        if let Some(keys) = scale {
            let sliced = super::actions::slice_keys(keys, false, start, end, &mut boundary);
            add_channel(gltf, &mut samplers, &mut targets, node, "scale", &sliced);
        }
        if let Some(keys) = rotation {
            let sliced = super::actions::slice_keys(keys, true, start, end, &mut boundary);
            let rotated = helper.unwrap_or(node);
            add_channel(
                gltf,
                &mut samplers,
                &mut targets,
                rotated,
                "rotation",
                &sliced,
            );
            // A retail rotation key replaces the authored matrix, scale included.
            let authored_scale = gltf.doc["nodes"][rotated]["scale"]
                .as_array()
                .is_some_and(|s| s.iter().any(|v| (v.as_f64().unwrap() - 1.).abs() > 1e-5));
            let reset = if helper.is_some() {
                gltf.reset_rotation_scale.contains(&node)
            } else {
                scale.is_none() && authored_scale
            };
            if reset {
                let ones: Vec<(u64, Vec<f32>)> =
                    sliced.iter().map(|(f, _)| (*f, vec![1., 1., 1.])).collect();
                add_channel(gltf, &mut samplers, &mut targets, rotated, "scale", &ones);
            }
        }
        gltf.stats.boundary_keys = boundary;
    }
    if morphs {
        let driven: BTreeSet<_> = channels
            .iter()
            .filter(|c| c["type"] == 12)
            .filter_map(|c| c["frame"].as_u64())
            .collect();
        for frame in driven {
            let frame = frame as usize;
            let node = node_offset + frame;
            let node = gltf.rotation_nodes.get(&node).copied().unwrap_or(node);
            let Some(mesh_id) = gltf.doc["nodes"][node]["mesh"].as_u64().map(|v| v as usize) else {
                continue;
            };
            let Some(morphs) = gltf.doc["meshes"][mesh_id]["extras"]["kazarov_morph_targets"]
                .as_array()
                .cloned()
            else {
                continue;
            };
            let vertex_channels: Vec<_> = channels
                .iter()
                .filter(|c| {
                    c["type"] == 12
                        && c["frame"].as_u64() == Some(frame as u64)
                        && c["keys"].as_array().is_some_and(|k| !k.is_empty())
                })
                .collect();
            if vertex_channels.is_empty() {
                continue;
            }
            let mut times: BTreeSet<_> = vertex_channels
                .iter()
                .flat_map(|c| c["keys"].as_array().unwrap())
                .filter_map(|k| k["frame"].as_u64())
                .filter(|f| (start..=end).contains(f))
                .collect();
            times.insert(start);
            times.insert(end);
            let mut values = Vec::new();
            for &time in &times {
                for morph in &morphs {
                    let vertex = morph["vertex"].as_u64().unwrap() + 1;
                    let axis = morph["axis"].as_u64().unwrap() as usize;
                    values.push(
                        vertex_channels
                            .iter()
                            .rev()
                            .find(|c| c["target"].as_u64() == Some(vertex))
                            .map_or(0., |c| {
                                sample_key(c["keys"].as_array().unwrap(), time, axis)
                                    - morph["base"].as_f64().unwrap() as f32
                            }),
                    );
                }
            }
            let input = gltf.animation_accessor(
                &times
                    .iter()
                    .map(|&t| (t - start) as f32 / 30.)
                    .collect::<Vec<_>>(),
                1,
                true,
            );
            let output = gltf.animation_accessor(&values, 1, false);
            samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
            targets
                .push(json!({"sampler":samplers.len()-1,"target":{"node":node,"path":"weights"}}));
        }
    }
    if targets.is_empty() {
        return Ok(None);
    }
    let animations = gltf.doc["animations"].as_array_mut().unwrap();
    animations.push(json!({"name":name,"samplers":samplers,"channels":targets}));
    Ok(Some(animations.len() - 1))
}

/// Last source frame of any key of a motion, the extent of a motion without KEY intervals.
fn motion_extent(channels: &[Value]) -> u64 {
    channels
        .iter()
        .flat_map(|c| c["keys"].as_array().into_iter().flatten())
        .filter_map(|k| k["frame"].as_u64())
        .max()
        .unwrap_or(0)
}

struct NativeArtifact {
    gltf: Value,
    metadata: Value,
    animations: Vec<Value>,
    packed: Option<PackedNativeArtifact>,
}

struct PackedNativeArtifact {
    bytes: Vec<u8>,
    decoded_len: usize,
}

impl NativeArtifact {
    fn pack(&mut self) -> CoreResult<()> {
        if self.packed.is_some() {
            return Ok(());
        }
        let encoded = serde_json::to_vec(&(&self.gltf, &self.metadata, &self.animations))?;
        let packed = zstd::bulk::compress(&encoded, 1)?;
        self.gltf = Value::Null;
        self.metadata = Value::Null;
        self.animations.clear();
        self.packed = Some(PackedNativeArtifact {
            bytes: packed,
            decoded_len: encoded.len(),
        });
        Ok(())
    }

    fn expanded(&self) -> CoreResult<Self> {
        let Some(packed) = &self.packed else {
            return Ok(Self {
                gltf: self.gltf.clone(),
                metadata: self.metadata.clone(),
                animations: self.animations.clone(),
                packed: None,
            });
        };
        let decoded = zstd::bulk::decompress(&packed.bytes, packed.decoded_len)?;
        if decoded.len() != packed.decoded_len {
            return Err(fail("native artifact decompressed length differs"));
        }
        let (gltf, metadata, animations) = serde_json::from_slice(&decoded)?;
        Ok(Self {
            gltf,
            metadata,
            animations,
            packed: None,
        })
    }
}

/// Describes native assets and texture containers consumed by model publication.
#[derive(Default)]
pub struct ConversionReport {
    pub files: Vec<String>,
    pub consumed_texture_sources: BTreeSet<String>,
    /// Port-relative standalone PNGs referenced by published glTF images.
    pub referenced_pngs: BTreeSet<String>,
    /// How animated retail transforms were expressed in standard glTF.
    pub transforms: TransformStats,
    /// Models external motions can drive.
    rigs: Vec<Rig>,
    /// Motions published into other bundles' models.
    externals: Vec<ExternalMotion>,
    /// External motions whose models could not be determined, with the evidence.
    pub unresolved: Vec<String>,
    /// Constant `_LOAD_CHARA` and `_LOAD_MOTION` calls of every compiled script, by script alias.
    script_loads: Vec<(String, super::native_scripts::ActorLoad)>,
    /// String literals of every compiled script, by script alias.
    script_literals: BTreeMap<String, BTreeSet<String>>,
    /// Models each external motion was published into, with the game code or data that plays it there.
    pub placements: Vec<Value>,
    /// Every source alias converted into native assets.
    pub converted: BTreeSet<String>,
}

fn character_parent(alias: &str) -> Option<&str> {
    alias
        .rmatch_indices('/')
        .map(|(at, _)| &alias[..at])
        .find(|s| is_character(s))
}
fn leaf_stem(alias: &str) -> String {
    stem(alias.rsplit('/').next().unwrap_or(alias)).to_owned()
}
fn shift(value: &mut Value, offset: usize) {
    if let Some(v) = value.as_u64() {
        *value = json!(v + offset as u64)
    }
}
fn shift_field(value: &mut Value, key: &str, offset: usize) {
    if let Some(field) = value.get_mut(key) {
        shift(field, offset);
    }
}
fn empty_gltf() -> Value {
    json!({"asset":{"version":"2.0","generator":"Kazarov"}})
}
fn merge_gltf(target: &mut Value, source: &Value) -> BTreeMap<&'static str, usize> {
    let keys = [
        "buffers",
        "bufferViews",
        "accessors",
        "images",
        "samplers",
        "textures",
        "materials",
        "meshes",
        "skins",
        "nodes",
        "scenes",
        "animations",
    ];
    let offsets: BTreeMap<_, _> = keys
        .iter()
        .map(|&key| (key, target[key].as_array().map_or(0, Vec::len)))
        .collect();
    for key in keys {
        let Some(values) = source[key].as_array() else {
            continue;
        };
        for item in values {
            let mut value = item.clone();
            match key {
                "bufferViews" => shift_field(&mut value, "buffer", offsets["buffers"]),
                "accessors" => {
                    shift_field(&mut value, "bufferView", offsets["bufferViews"]);
                    if value["sparse"].is_object() {
                        shift(
                            &mut value["sparse"]["indices"]["bufferView"],
                            offsets["bufferViews"],
                        );
                        shift(
                            &mut value["sparse"]["values"]["bufferView"],
                            offsets["bufferViews"],
                        );
                    }
                }
                "textures" => {
                    shift_field(&mut value, "source", offsets["images"]);
                    shift_field(&mut value, "sampler", offsets["samplers"]);
                }
                "materials" => {
                    for texture in ["baseColorTexture", "metallicRoughnessTexture"] {
                        if value["pbrMetallicRoughness"][texture].is_object() {
                            shift(
                                &mut value["pbrMetallicRoughness"][texture]["index"],
                                offsets["textures"],
                            );
                        }
                    }
                    for texture in ["normalTexture", "occlusionTexture", "emissiveTexture"] {
                        if value[texture].is_object() {
                            shift(&mut value[texture]["index"], offsets["textures"]);
                        }
                    }
                }
                "meshes" => {
                    for primitive in value["primitives"].as_array_mut().unwrap() {
                        for a in primitive["attributes"]
                            .as_object_mut()
                            .unwrap()
                            .values_mut()
                        {
                            shift(a, offsets["accessors"]);
                        }
                        shift_field(primitive, "indices", offsets["accessors"]);
                        shift_field(primitive, "material", offsets["materials"]);
                        if let Some(targets) =
                            primitive.get_mut("targets").and_then(Value::as_array_mut)
                        {
                            for morph in targets {
                                for a in morph.as_object_mut().unwrap().values_mut() {
                                    shift(a, offsets["accessors"]);
                                }
                            }
                        }
                    }
                }
                "skins" => {
                    for joint in value["joints"].as_array_mut().unwrap() {
                        shift(joint, offsets["nodes"]);
                    }
                    shift_field(&mut value, "skeleton", offsets["nodes"]);
                    shift_field(&mut value, "inverseBindMatrices", offsets["accessors"]);
                }
                "nodes" => {
                    shift_field(&mut value, "mesh", offsets["meshes"]);
                    shift_field(&mut value, "skin", offsets["skins"]);
                    if let Some(children) = value.get_mut("children").and_then(Value::as_array_mut)
                    {
                        for child in children {
                            shift(child, offsets["nodes"]);
                        }
                    }
                }
                "scenes" => {
                    if let Some(nodes) = value.get_mut("nodes").and_then(Value::as_array_mut) {
                        for node in nodes {
                            shift(node, offsets["nodes"]);
                        }
                    }
                }
                "animations" => {
                    for sampler in value["samplers"].as_array_mut().unwrap() {
                        shift(&mut sampler["input"], offsets["accessors"]);
                        shift(&mut sampler["output"], offsets["accessors"]);
                    }
                    for channel in value["channels"].as_array_mut().unwrap() {
                        shift(&mut channel["target"]["node"], offsets["nodes"]);
                    }
                }
                _ => {}
            }
            target
                .as_object_mut()
                .unwrap()
                .entry(key)
                .or_insert_with(|| json!([]))
                .as_array_mut()
                .unwrap()
                .push(value);
        }
    }
    if target["scenes"].as_array().is_some_and(|v| !v.is_empty()) {
        target["scene"] = json!(0);
    }
    offsets
}
fn remap_image_metadata(value: &mut Value, offset: usize) {
    if let Some(pictures) = value.as_array_mut() {
        for picture in pictures {
            if let Some(levels) = picture.get_mut("levels").and_then(Value::as_array_mut) {
                for image in levels {
                    shift(image, offset);
                }
            }
            if let Some(variants) = picture.get_mut("variants").and_then(Value::as_array_mut) {
                for variant in variants {
                    if let Some(levels) = variant.get_mut("levels").and_then(Value::as_array_mut) {
                        for image in levels {
                            shift(image, offset);
                        }
                    }
                }
            }
        }
    }
}
fn embed_texture_set(
    gltf: &mut Value,
    published: &Path,
    source: &super::textures::ConvertedTextureDocument,
) -> CoreResult<Value> {
    let mut doc = source.document.clone();
    let directory = published
        .join("data")
        .join(Path::new(&source.document_path).parent().unwrap());
    fn levels(gltf: &mut Value, directory: &Path, value: &mut Value) -> CoreResult<()> {
        if let Some(levels) = value.as_array_mut() {
            for level in levels {
                let name = level
                    .as_str()
                    .ok_or_else(|| fail("texture image path missing"))?;
                let images = gltf
                    .as_object_mut()
                    .unwrap()
                    .entry("images")
                    .or_insert_with(|| json!([]))
                    .as_array_mut()
                    .unwrap();
                let png = png_identity(&directory.join(name));
                let image = if let Some(image) = images
                    .iter()
                    .position(|image| image["extras"]["chronicle_two"]["png"] == png)
                {
                    image
                } else {
                    let image = images.len();
                    images.push(json!({"name":name,"extras":{"chronicle_two":{"png":png}}}));
                    image
                };
                *level = json!(format!("#/images/{image}"));
            }
        }
        Ok(())
    }
    for entry in doc["entries"]
        .as_array_mut()
        .ok_or_else(|| fail("texture set entries missing"))?
    {
        let Some(pictures) = entry.get_mut("pictures").and_then(Value::as_array_mut) else {
            if entry["script"].is_array() {
                continue;
            } else {
                return Err(fail("texture set pictures missing"));
            }
        };
        for picture in pictures {
            if let Some(value) = picture.get_mut("levels") {
                levels(gltf, &directory, value)?;
            }
            if let Some(variants) = picture.get_mut("variants").and_then(Value::as_array_mut) {
                for variant in variants {
                    let value = variant
                        .get_mut("levels")
                        .ok_or_else(|| fail("texture variant levels missing"))?;
                    levels(gltf, &directory, value)?;
                }
            }
        }
    }
    Ok(json!({"name":leaf_stem(&source.source_alias),"document":doc}))
}
fn resource_operand(command: &str, arg: usize) -> Option<&'static str> {
    match command {
        "MODEL" | "SHADOW_MODEL" if arg == 0 => Some("model"),
        "IMG" if arg == 1 => Some("texture_set"),
        "MOTION" => match arg {
            1 => Some("animation"),
            2 => Some("bind_pose"),
            3 => Some("weights"),
            _ => None,
        },
        "SHADOW_MOTION" => match arg {
            1 => Some("bind_pose"),
            2 => Some("weights"),
            _ => None,
        },
        "LOD_MODEL" => match arg {
            1 => Some("model"),
            2 => Some("weights"),
            3 => Some("bind_pose"),
            _ => None,
        },
        "CLOTH" => Some("cloth"),
        "EFFECT_START" if arg == 0 => Some("character"),
        "EFFECT" if arg == 0 => Some("script"),
        _ => None,
    }
}
/// Rewrites one profile for native loading: typed resources, English action names and local timing.
///
/// KEY keeps its English action name and step; the action's clip supplies its length. Sound frames
/// given as absolute source frames become ratios of their named action.
#[allow(clippy::too_many_arguments)]
fn rewrite_profile(
    mut document: Value,
    profile: &str,
    container: &str,
    references: &BTreeMap<(String, String), usize>,
    book: &mut NameBook,
    unbound: &BTreeMap<String, usize>,
) -> CoreResult<Value> {
    let commands = document["commands"]
        .as_array_mut()
        .ok_or_else(|| fail("character profile commands missing"))?;
    let mut intervals = BTreeMap::<String, (u64, u64)>::new();
    for statement in commands.iter() {
        if statement["command"] == "KEY" {
            let args = &statement["args"];
            if let (Some(name), Some(start), Some(end)) = (
                args[0]["value"].as_str(),
                args[1]["value"].as_u64(),
                args[2]["value"].as_u64(),
            ) {
                intervals.entry(name.to_owned()).or_insert((start, end));
            }
        }
    }
    let mut effect_container = container.to_owned();
    let mut motion = String::new();
    for (index, statement) in commands.iter_mut().enumerate() {
        let command = statement["command"].as_str().unwrap_or("").to_owned();
        let args = statement["args"]
            .as_array_mut()
            .ok_or_else(|| fail("character arguments missing"))?;
        match command.as_str() {
            "MOTION" => {
                let id = super::actions::unbound_motion(profile, index);
                motion = match args.get(1).and_then(|a| a["value"].as_str()) {
                    Some(value) if !value.is_empty() => {
                        format!("{container}/{}", value.replace('\\', "/"))
                    }
                    _ => id.clone(),
                };
                if let Some(&resource) = unbound.get(&motion) {
                    args[1] = json!({"type":"resource","kind":"animation","index":resource});
                }
            }
            "KEY" => {
                let start = args[1]["value"].as_u64().unwrap_or(0);
                let action = KeyAction {
                    game: args[0].clone(),
                    start,
                    end: args[2]["value"].as_u64().unwrap_or(0).max(start),
                };
                let name = book.action(&motion, &action, "")?;
                let step = args
                    .get(3)
                    .cloned()
                    .ok_or_else(|| fail(format!("KEY without step in {profile}")))?;
                *args = vec![json!({"type":"string","value":name}), step];
                continue;
            }
            "SE" | "SELP" => {
                let game = args
                    .first()
                    .cloned()
                    .ok_or_else(|| fail(format!("{command} without motion in {profile}")))?;
                let text = game["value"].as_str().unwrap_or("").to_owned();
                let frames: &[usize] = if command == "SE" { &[3] } else { &[3, 4] };
                for &at in frames {
                    let Some(frame) = args.get(at).and_then(|a| a["value"].as_f64()) else {
                        continue;
                    };
                    if frame > 1. {
                        let &(start, end) = intervals.get(&text).ok_or_else(|| {
                            fail(format!("absolute sound frame names no KEY: {text}"))
                        })?;
                        args[at] = super::actions::ratio(frame, start, end);
                    }
                }
                args[0] = json!({"type":"string","value":book.reference(&game)?});
                continue;
            }
            "SEQ_START" | "SEQ" => {
                if let Some(game) = args.first().cloned().filter(|a| a["type"] == "string") {
                    args[0] = json!({"type":"string","value":book.reference(&game)?});
                }
                continue;
            }
            _ => {}
        }
        for (index, arg) in args.iter_mut().enumerate() {
            let Some(kind) = resource_operand(&command, index) else {
                continue;
            };
            let Some(value) = arg["value"].as_str() else {
                continue;
            };
            if value.is_empty() {
                continue;
            }
            let source = if command == "EFFECT" {
                &effect_container
            } else {
                container
            };
            let alias = format!("{source}/{}", value.replace('\\', "/"));
            let reference = references.get(&(kind.to_owned(), alias.clone())).copied();
            *arg = json!({"type":"resource","kind":kind,"index":reference});
            if command == "EFFECT_START" {
                effect_container = alias;
            }
        }
    }
    Ok(json!({"commands":document["commands"]}))
}

/// A motion whose skeleton is another bundle's model; its actions are published into those models.
#[derive(Clone)]
pub(crate) struct ExternalMotion {
    /// Identity shared by the referencing resource and the entries published into target models.
    key: String,
    /// Published sidecar of the referencing character bundle, when there is one.
    sidecar: Option<String>,
    /// Index of the referencing animation resource in that sidecar.
    resource: usize,
    /// Retail motion channels.
    channels: Vec<Value>,
    /// English action names with their source intervals.
    actions: Vec<(String, u64, u64)>,
}

/// One published character model that external motions can drive.
#[derive(Clone)]
pub(crate) struct Rig {
    /// Published glTF path.
    path: String,
    /// Model index in the character sidecar.
    model: usize,
    /// First standard node of the model.
    node_offset: usize,
    /// Native frame names, identifying the skeleton family.
    names: Vec<String>,
    /// Rest translation of every native frame.
    translations: Vec<[f32; 3]>,
    /// Shear helper of each native frame that has one.
    helpers: BTreeMap<usize, usize>,
}

/// Names an action's glTF clip, telling apart the clips of a motion shared by several models.
fn clip_name(name: &str, model: usize, scene: &str) -> String {
    if model == 0 {
        name.to_owned()
    } else {
        format!("{name}@{scene}")
    }
}

/// Source aliases `publish_native` consumes into the characters among `resources`.
fn native_claims(resources: &[Resource]) -> Vec<String> {
    let mut claims = Vec::new();
    for character in resources.iter().filter(|r| is_character(&r.alias)) {
        let alias = character.alias.as_str();
        claims.extend(
            resources
                .iter()
                .filter(|r| {
                    character_parent(&r.alias) == Some(alias)
                        && matches!(
                            extension(&r.alias),
                            "mot" | "bbp" | "wgt" | "clo" | "stb" | "chr" | "chr_" | "cfg"
                        )
                })
                .map(|r| r.alias.clone()),
        );
        claims.push(alias.to_owned());
    }
    claims
}

fn publish_native(
    published: &Path,
    resources: &[Resource],
    artifacts: &BTreeMap<String, NativeArtifact>,
    documents: &[super::textures::ConvertedTextureDocument],
    report: &mut ConversionReport,
) -> CoreResult<()> {
    publish_native_impl(published, resources, artifacts, documents, report, None)
}

fn publish_native_impl(
    published: &Path,
    resources: &[Resource],
    artifacts: &BTreeMap<String, NativeArtifact>,
    documents: &[super::textures::ConvertedTextureDocument],
    report: &mut ConversionReport,
    mut stream: Option<&mut StreamPairs<'_>>,
) -> CoreResult<()> {
    let decoded = if artifacts.values().any(|artifact| artifact.packed.is_some()) {
        Some(
            artifacts
                .iter()
                .map(|(name, artifact)| Ok((name.clone(), artifact.expanded()?)))
                .collect::<CoreResult<BTreeMap<_, _>>>()?,
        )
    } else {
        None
    };
    let artifacts = decoded.as_ref().unwrap_or(artifacts);
    let characters: Vec<_> = resources
        .iter()
        .filter(|r| is_character(&r.alias))
        .collect();
    let mut emitted = BTreeSet::new();
    for (source, artifact) in artifacts {
        let output = super::configs::native_path(source, "gltf");
        let Some(character) = characters
            .iter()
            .find(|character| super::configs::native_path(&character.alias, "gltf") == output)
        else {
            continue;
        };
        let original = resources
            .iter()
            .find(|resource| resource.alias == *source)
            .unwrap();
        let duplicated_geometry = resources.iter().any(|resource| {
            character_parent(&resource.alias) == Some(character.alias.as_str())
                && matches!(extension(&resource.alias), "mds" | "mdt")
                && resource.data == original.data
        });
        let has_auxiliary = !artifact.animations.is_empty()
            || ["motion_channels", "weights", "bind_poses"]
                .iter()
                .any(|key| {
                    artifact.metadata[*key]
                        .as_array()
                        .is_some_and(|values| !values.is_empty())
                });
        if !duplicated_geometry || has_auxiliary {
            return Err(fail(format!(
                "native character/model output collision: {} and {source} -> {output}",
                character.alias
            )));
        }
        emitted.insert(source.clone());
    }
    for character in &characters {
        let alias = &character.alias;
        let path = super::configs::native_path(alias, "gltf");
        let sidecar = Path::new(&path)
            .with_extension("game.json")
            .to_string_lossy()
            .replace('\\', "/");
        let mut profiles = Vec::new();
        let keyed = if stream
            .as_ref()
            .is_some_and(|output| output.gameplay.is_none())
        {
            if let Some(plan) = super::configs::profile_actions_for(alias)? {
                plan.actions.clone()
            } else if resources.iter().any(|resource| {
                extension(&resource.alias).eq_ignore_ascii_case("cfg")
                    && character_parent(&resource.alias) == Some(alias)
            }) {
                return Err(fail(format!(
                    "missing checked-in gameplay clip plan for {alias}"
                )));
            } else {
                super::actions::ProfileActions::default()
            }
        } else {
            for profile in resources.iter().filter(|r| {
                extension(&r.alias) == "cfg" && character_parent(&r.alias) == Some(alias.as_str())
            }) {
                profiles.push((profile.alias.clone(), super::configs::parse(&profile.data)?));
            }
            super::actions::key_actions(&profiles)?
        };
        let mut book = NameBook::default();
        for (motion, action) in &keyed.order {
            book.action(motion, action, "")?;
        }
        let mut gltf = empty_gltf();
        let mut models = Vec::new();
        let mut references = BTreeMap::new();
        let mut animation_variants = BTreeMap::<String, Vec<Value>>::new();
        let mut selected_textures = BTreeSet::new();
        for (source, artifact) in artifacts
            .iter()
            .filter(|(source, _)| character_parent(source) == Some(alias.as_str()))
        {
            let offsets = merge_gltf(&mut gltf, &artifact.gltf);
            let model_id = if artifact.metadata["frames"]
                .as_array()
                .is_some_and(|a| !a.is_empty())
            {
                let id = models.len();
                let mut metadata = artifact.metadata.clone();
                metadata.as_object_mut().unwrap().remove("source");
                metadata["name"] = json!(leaf_stem(source));
                metadata["scene"] = json!(offsets["scenes"]);
                metadata["node_offset"] = json!(offsets["nodes"]);
                for frame in metadata["frames"].as_array_mut().unwrap() {
                    shift_field(frame, "rotation_node", offsets["nodes"]);
                }
                for mesh in metadata["meshes"].as_array_mut().unwrap() {
                    for material in mesh["materials"].as_array_mut().unwrap() {
                        if let Some(source) = material["texture_source"].as_str() {
                            selected_textures.insert(source.to_owned());
                        }
                        remap_image_metadata(&mut material["texture_images"], offsets["images"]);
                    }
                }
                if let Some(scene) = gltf["scenes"]
                    .as_array_mut()
                    .and_then(|a| a.get_mut(offsets["scenes"]))
                {
                    scene["name"] = json!(leaf_stem(source));
                }
                references.insert(("model".into(), source.clone()), id);
                let frames = metadata["frames"].as_array().unwrap();
                let node = |frame: usize| &gltf["nodes"][offsets["nodes"] + frame];
                report.rigs.push(Rig {
                    path: path.clone(),
                    model: id,
                    node_offset: offsets["nodes"],
                    names: (0..frames.len())
                        .map(|f| node(f)["name"].as_str().unwrap_or("").to_owned())
                        .collect(),
                    translations: (0..frames.len())
                        .map(|f| {
                            let n = node(f);
                            let t = n.get("matrix").and_then(Value::as_array).map_or_else(
                                || n.get("translation").cloned().unwrap_or(json!([0, 0, 0])),
                                |m| json!(&m[12..15]),
                            );
                            std::array::from_fn(|i| t[i].as_f64().unwrap_or(0.) as f32)
                        })
                        .collect(),
                    helpers: frames
                        .iter()
                        .enumerate()
                        .filter_map(|(f, frame)| {
                            frame["rotation_node"].as_u64().map(|h| (f, h as usize))
                        })
                        .collect(),
                });
                models.push(metadata);
                Some(id)
            } else {
                None
            };
            for animation in &artifact.animations {
                let (Some(motion), Some(model)) = (animation["alias"].as_str(), model_id) else {
                    continue;
                };
                let previous = animation_variants.get(motion).map_or(0, Vec::len);
                let mut clips = Vec::new();
                for clip in animation["clips"].as_array().unwrap() {
                    let action = KeyAction {
                        game: clip["game"].clone(),
                        start: clip["start"].as_u64().unwrap(),
                        end: clip["end"].as_u64().unwrap(),
                    };
                    let name = book.action(motion, &action, &leaf_stem(motion))?;
                    let index = clip["index"]
                        .as_u64()
                        .map(|index| index as usize + offsets["animations"]);
                    if let Some(index) = index {
                        gltf["animations"][index]["name"] =
                            json!(clip_name(&name, previous, &leaf_stem(source)));
                    }
                    clips.push(json!(index));
                }
                animation_variants
                    .entry(motion.into())
                    .or_default()
                    .push(json!({"model":model,"node_offset":offsets["nodes"],"animations":clips}));
            }
            emitted.insert(source.clone());
        }
        let mut native = json!({"schema_version":1,"kind":"character","models":models,"profiles":{},"resources":{"animations":[],"bind_poses":[],"weights":[],"cloths":[],"scripts":[],"children":[],"texture_sets":[]}});
        for resource in resources
            .iter()
            .filter(|r| character_parent(&r.alias) == Some(alias.as_str()))
        {
            let kind = match extension(&resource.alias) {
                "mot" => "animation",
                "bbp" => "bind_pose",
                "wgt" => "weights",
                "clo" => "cloth",
                "stb" => "script",
                "chr" | "chr_" => "character",
                _ => continue,
            };
            let key = match kind {
                "animation" => "animations",
                "bind_pose" => "bind_poses",
                "weights" => "weights",
                "cloth" => "cloths",
                "script" => "scripts",
                _ => "children",
            };
            let index = native["resources"][key].as_array().unwrap().len();
            let mut value = json!({});
            match kind {
                "animation" => {
                    let channels = channels(resource)?;
                    let list = keyed
                        .motions
                        .get(&resource.alias)
                        .cloned()
                        .unwrap_or_else(|| {
                            vec![KeyAction {
                                game: Value::Null,
                                start: 0,
                                end: motion_extent(&channels),
                            }]
                        });
                    let mut actions = Vec::new();
                    let mut named = Vec::new();
                    for action in &list {
                        let name =
                            book.action(&resource.alias, action, &leaf_stem(&resource.alias))?;
                        let auxiliary: Vec<Value> = channels
                            .iter()
                            .filter(|c| c["type"].as_i64().is_some_and(|t| t > 2))
                            .map(|c| super::actions::slice_aux(c, action.start, action.end))
                            .collect();
                        actions.push(json!({"name":name,"duration":action.end - action.start,"channels":auxiliary}));
                        named.push((name, action.start, action.end));
                    }
                    let variants = animation_variants
                        .remove(&resource.alias)
                        .unwrap_or_default();
                    let transforms = channels
                        .iter()
                        .any(|c| matches!(c["type"].as_i64(), Some(0..=2 | 12)));
                    if variants.is_empty() && transforms {
                        let external = format!("{path}#{index}");
                        for action in &mut actions {
                            action.as_object_mut().unwrap().remove("channels");
                        }
                        value = json!({"external":external,"actions":actions,"targets":[]});
                        report.externals.push(ExternalMotion {
                            key: external,
                            sidecar: Some(sidecar.clone()),
                            resource: index,
                            channels,
                            actions: named,
                        });
                    } else {
                        value = json!({"actions":actions,"variants":variants});
                    }
                }
                "bind_pose" => value["matrices"] = bind_pose(resource)?["matrices"].clone(),
                "weights" => value["channels"] = json!(channels(resource)?),
                "cloth" => {
                    if stream.is_none()
                        || stream
                            .as_ref()
                            .is_some_and(|output| output.gameplay.is_some())
                    {
                        value["commands"] =
                            super::configs::parse(&resource.data)?["commands"].clone();
                    }
                }
                "script" => {
                    let path = super::configs::native_path(&resource.alias, "stb");
                    let dest = published.join("data").join(&path);
                    if stream.is_none() {
                        fs::create_dir_all(dest.parent().unwrap())?;
                        fs::write(dest, &resource.data)?;
                    }
                    value["path"] = json!(path.clone());
                    value["texture_sets"] = json!([]);
                    report.files.push(path);
                }
                "character" => {
                    value["path"] = json!(super::configs::native_path(&resource.alias, "gltf"))
                }
                _ => {}
            }
            native["resources"][key].as_array_mut().unwrap().push(value);
            references.insert((kind.into(), resource.alias.clone()), index);
        }
        // KEY intervals of a MOTION naming no motion, or a motion absent from the bundle, keep their
        // actions without clips.
        let mut unbound = BTreeMap::new();
        let missing = keyed
            .motions
            .iter()
            .filter(|(motion, _)| {
                !references.contains_key(&("animation".to_owned(), (*motion).clone()))
            })
            .map(|(motion, list)| (motion.clone(), list));
        let unbound_lists = keyed.unbound.iter().map(|((profile, command), list)| {
            (super::actions::unbound_motion(profile, *command), list)
        });
        for (id, list) in missing.chain(unbound_lists).collect::<Vec<_>>() {
            let mut actions = Vec::new();
            for action in list {
                let name = book.action(&id, action, "")?;
                actions.push(json!({"name":name,"duration":action.end - action.start}));
            }
            let index = native["resources"]["animations"].as_array().unwrap().len();
            native["resources"]["animations"]
                .as_array_mut()
                .unwrap()
                .push(json!({"actions":actions,"variants":[]}));
            unbound.insert(id, index);
        }
        for source in documents.iter().filter(|d| {
            character_parent(&d.source_alias) == Some(alias.as_str())
                || selected_textures.contains(&d.source_alias)
        }) {
            let index = native["resources"]["texture_sets"]
                .as_array()
                .unwrap()
                .len();
            native["resources"]["texture_sets"]
                .as_array_mut()
                .unwrap()
                .push(embed_texture_set(&mut gltf, published, source)?);
            references.insert(("texture_set".into(), source.source_alias.clone()), index);
            if character_parent(&source.source_alias) == Some(alias.as_str()) {
                report
                    .consumed_texture_sources
                    .insert(source.source_alias.clone());
            }
        }
        let texture_count = native["resources"]["texture_sets"]
            .as_array()
            .unwrap()
            .len();
        for model in native["models"].as_array_mut().unwrap() {
            let mut dependencies = BTreeSet::new();
            for mesh in model["meshes"].as_array().unwrap() {
                for material in mesh["materials"].as_array().unwrap() {
                    if let Some(source) = material["texture_source"].as_str()
                        && let Some(index) = references.get(&("texture_set".into(), source.into()))
                    {
                        dependencies.insert(*index);
                    }
                }
            }
            if !dependencies.is_empty() {
                model["texture_sets"] = json!(dependencies);
            }
        }
        for script in native["resources"]["scripts"].as_array_mut().unwrap() {
            script["texture_sets"] = json!((0..texture_count).collect::<Vec<_>>());
        }
        // Effect scripts are direct files even when their effect character is a child bundle.
        for resource in resources
            .iter()
            .filter(|r| extension(&r.alias) == "stb" && r.alias.starts_with(&format!("{alias}/")))
        {
            let key = ("script".into(), resource.alias.clone());
            references.entry(key).or_insert_with(|| {
                let index = native["resources"]["scripts"].as_array().unwrap().len();
                native["resources"]["scripts"].as_array_mut().unwrap().push(json!({"name":leaf_stem(&resource.alias),"path":super::configs::native_path(&resource.alias,"stb"),"character":super::configs::native_path(character_parent(&resource.alias).unwrap(),"gltf")}));
                index
            });
        }
        if stream.is_none()
            || stream
                .as_ref()
                .is_some_and(|output| output.gameplay.is_some())
        {
            for (profile, document) in profiles {
                let name = leaf_stem(&profile);
                if native.get("default_profile").is_none() {
                    native["default_profile"] = json!(name);
                }
                native["profiles"][&name] =
                    rewrite_profile(document, &profile, alias, &references, &mut book, &unbound)?;
            }
            let names: serde_json::Map<String, Value> = book
                .names
                .iter()
                .filter(|(_, game)| !game.is_null())
                .map(|(name, game)| (name.clone(), game.clone()))
                .collect();
            if !names.is_empty() {
                native["game_names"] = Value::Object(names);
            }
        }
        publish_pair_impl(
            published,
            &path,
            &mut gltf,
            &mut native,
            report,
            stream.as_deref_mut(),
        )?;
    }
    let mut standalone = BTreeMap::<String, Vec<(&String, &NativeArtifact)>>::new();
    for (alias, artifact) in artifacts
        .iter()
        .filter(|(alias, _)| !emitted.contains(*alias))
    {
        standalone
            .entry(super::configs::native_path(alias, "gltf"))
            .or_default()
            .push((alias, artifact));
    }
    for (path, mut entries) in standalone {
        entries.sort_by_key(|(_, a)| a.metadata["frames"].as_array().is_none_or(Vec::is_empty));
        let mut gltf = empty_gltf();
        let mut metadata = entries[0].1.metadata.clone();
        let mut selected_textures = BTreeSet::new();
        for (i, (_, artifact)) in entries.iter().enumerate() {
            let offsets = merge_gltf(&mut gltf, &artifact.gltf);
            if i == 0 {
                for mesh in metadata["meshes"].as_array_mut().unwrap() {
                    for material in mesh["materials"].as_array_mut().unwrap() {
                        if let Some(source) = material["texture_source"].as_str() {
                            selected_textures.insert(source.to_owned());
                        }
                        remap_image_metadata(&mut material["texture_images"], offsets["images"]);
                    }
                }
            } else {
                for key in ["motion_channels", "weights", "bind_poses"] {
                    metadata[key]
                        .as_array_mut()
                        .unwrap()
                        .extend(artifact.metadata[key].as_array().unwrap().clone());
                }
            }
        }
        metadata["kind"] = json!(
            if metadata["frames"].as_array().is_some_and(|a| !a.is_empty()) {
                "model"
            } else {
                "animation"
            }
        );
        if !selected_textures.is_empty() {
            let mut sets = Vec::new();
            for source in documents
                .iter()
                .filter(|d| selected_textures.contains(&d.source_alias))
            {
                sets.push(embed_texture_set(&mut gltf, published, source)?);
            }
            metadata["texture_sets"] = json!(sets);
        }
        publish_pair_impl(
            published,
            &path,
            &mut gltf,
            &mut metadata,
            report,
            stream.as_deref_mut(),
        )?;
    }
    Ok(())
}

/// Scores how well a motion's translation keys match each skeleton family's rest translations.
///
/// Families are models with identical native frame names. Returns the best member score of each.
fn family_scores(motion: &ExternalMotion, rigs: &[Rig]) -> Vec<(f32, Vec<usize>)> {
    let keys = transform_keys(&motion.channels);
    let needed = keys.keys().max().map_or(0, |frame| frame + 1);
    let translations: Vec<(usize, [f32; 3])> = keys
        .iter()
        .filter_map(|(&frame, [_, _, translation])| {
            translation.map(|keys| {
                (
                    frame,
                    std::array::from_fn(|i| keys[0]["value"][i].as_f64().unwrap_or(0.) as f32),
                )
            })
        })
        .collect();
    let mut families = BTreeMap::<&[String], Vec<usize>>::new();
    for (index, rig) in rigs.iter().enumerate() {
        if rig.names.len() >= needed {
            families.entry(&rig.names).or_default().push(index);
        }
    }
    families
        .into_values()
        .map(|members| {
            let best = members
                .iter()
                .map(|&member| {
                    let rest = &rigs[member].translations;
                    let matched = translations
                        .iter()
                        .filter(|(frame, value)| {
                            let r = rest[*frame];
                            let length = r.iter().map(|v| v * v).sum::<f32>().sqrt();
                            (0..3)
                                .map(|i| (r[i] - value[i]).powi(2))
                                .sum::<f32>()
                                .sqrt()
                                <= 1e-3 * length.max(1.)
                        })
                        .count();
                    matched as f32 / translations.len().max(1) as f32
                })
                .fold(0., f32::max);
            (if translations.is_empty() { 0. } else { best }, members)
        })
        .collect()
}

/// Character packs `SetupMainUnit` can load into the scene's player unit (actor 0).
///
/// `GetMainCharaModelName` picks `mainchr/c01_base<2+n>` for Maximilian and `mainchr/c02_base<n>` for
/// Monica, where `n` is the melee weapon's model number clamped to 0..=3; edit mode loads the
/// `chara/` base model instead. Which one is loaded depends on the active character and weapon.
const MAIN_UNIT: [&str; 10] = [
    "mainchr/c01_base2.chr",
    "mainchr/c01_base3.chr",
    "mainchr/c01_base4.chr",
    "mainchr/c01_base5.chr",
    "chara/c01_base.chr",
    "mainchr/c02_base0.chr",
    "mainchr/c02_base1.chr",
    "mainchr/c02_base2.chr",
    "mainchr/c02_base3.chr",
    "chara/c02_base.chr",
];
/// Character that game code, rather than a script, loads an external motion pack into.
enum CodeOwner {
    /// The scene's player character (`scene->player_chara`, actor 0).
    MainUnit,
    /// A fixed character pack.
    Pack(&'static str),
}
/// External motion packs loaded by game code, with their owner and the loading function.
const CODE_LINKS: [(&str, CodeOwner, &str); 6] = [
    (
        "chara/c01_fishing.chr",
        CodeOwner::MainUnit,
        "fishing.cpp LoadExMotionStep and the dungeon fishing load step: scene player character",
    ),
    (
        "sg/pb/pb.pak/mints_bomb.chr",
        CodeOwner::MainUnit,
        "pbuggy.cpp buggy setup: scene->player_chara->LoadPack",
    ),
    (
        "menu/chara4/camera.pac/c01_camera.chr",
        CodeOwner::Pack("menu/chara4/c01b_think.chr"),
        "inventmn.cpp CMenuInvent::LoadCharaCheck: MenuActionChara[0] (invent mode loads c01b_think)",
    ),
    (
        "menu/chara4/c01_success.chr",
        CodeOwner::Pack("menu/chara4/c01b_think.chr"),
        "inventmn.cpp CMenuInvent::IsCreateObject: MenuActionChara[0] (invent mode loads c01b_think)",
    ),
    (
        "menu/chara4/c01_regret.chr",
        CodeOwner::Pack("menu/chara4/c01b_think.chr"),
        "inventmn.cpp CMenuInvent::IsCreateObject: MenuActionChara[0] (invent mode loads c01b_think)",
    ),
    (
        "menu/chara4/c01_failure.chr",
        CodeOwner::Pack("menu/chara4/c01b_think.chr"),
        "inventmn.cpp CMenuInvent::IsCreateObject: MenuActionChara[0] (invent mode loads c01b_think)",
    ),
];
/// Keeps the candidate models whose skeleton the motion animates.
///
/// A model fits when it has every frame the motion keys and its skeleton family scores within
/// 0.2 of the best candidate family, with the best matching at least half of the translation keys.
fn fitting_rigs(
    motion: &ExternalMotion,
    rigs: &[Rig],
    candidates: &BTreeSet<usize>,
) -> BTreeSet<usize> {
    let subset: Vec<Rig> = candidates.iter().map(|&i| rigs[i].clone()).collect();
    let index: Vec<usize> = candidates.iter().copied().collect();
    let scores = family_scores(motion, &subset);
    let best = scores.iter().map(|(score, _)| *score).fold(0., f32::max);
    if best < 0.5 {
        return BTreeSet::new();
    }
    scores
        .iter()
        .filter(|(score, _)| *score >= best - 0.2)
        .flat_map(|(_, members)| members.iter().map(|&m| index[m]))
        .collect()
}

fn linked_copy_candidates(
    externals: &[ExternalMotion],
    placements: &[Value],
    selected: &[BTreeSet<usize>],
    bundle: &str,
) -> (BTreeSet<usize>, Vec<String>) {
    let leaf = bundle.rsplit('/').next().unwrap_or(bundle);
    let mut copies = BTreeSet::new();
    let mut names = Vec::new();
    for (copy, other) in externals.iter().enumerate() {
        let path = other
            .key
            .split('#')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        if path != bundle
            && path.ends_with(&format!("/{leaf}"))
            && placements[copy]["method"] == "linked"
        {
            names.push(other.key.clone());
            copies.extend(selected[copy].iter().copied());
        }
    }
    (copies, names)
}

/// Chooses the models each external motion is published into from the game's own loads.
///
/// Event scripts bind a motion pack to an actor with `_LOAD_MOTION`; actor 0 is the scene's player
/// unit and every other actor is the pack the same script function last loaded into it with
/// `_LOAD_CHARA` (or any `_LOAD_CHARA` of that actor in the script when the function has none).
/// A few packs are loaded by game code listed in `CODE_LINKS`. Runtime-chosen owners, such as the
/// player unit, contribute every model the game can load there that the motion's skeleton fits.
/// A motion no load reaches is published by skeleton match over all models only when some script
/// names its pack; one nothing names is not published. Both are reported.
fn place_externals(report: &mut ConversionReport) -> Vec<BTreeSet<usize>> {
    let published = |raw: &str| super::configs::native_path(raw, "gltf").to_ascii_lowercase();
    let mut by_path = BTreeMap::<String, BTreeSet<usize>>::new();
    for (index, rig) in report.rigs.iter().enumerate() {
        by_path
            .entry(rig.path.to_ascii_lowercase())
            .or_default()
            .insert(index);
    }
    let main_unit: BTreeSet<usize> = MAIN_UNIT
        .iter()
        .filter_map(|pack| by_path.get(&published(pack)))
        .flatten()
        .copied()
        .collect();
    let mut leaves = BTreeMap::<String, BTreeSet<String>>::new();
    for path in report
        .externals
        .iter()
        .map(|m| m.key.split('#').next().unwrap_or("").to_ascii_lowercase())
        .chain(by_path.keys().cloned())
    {
        leaves
            .entry(path.rsplit('/').next().unwrap_or(&path).to_owned())
            .or_default()
            .insert(path);
    }
    let resolve = |raw: &str| -> BTreeSet<String> {
        let path = published(raw);
        if raw.contains('/') {
            return BTreeSet::from([path]);
        }
        leaves.get(&path).cloned().unwrap_or_default()
    };
    // Every character pack a script loads into an actor, by script, function and actor.
    let mut characters = BTreeMap::<(&str, i32), Vec<&super::native_scripts::ActorLoad>>::new();
    for (script, load) in report.script_loads.iter().filter(|(_, l)| !l.motion) {
        characters
            .entry((script.as_str(), load.actor))
            .or_default()
            .push(load);
    }
    let mut placements = Vec::new();
    let mut unresolved = Vec::new();
    let mut orphans = Vec::new();
    let mut out = Vec::new();
    for motion in &report.externals {
        let bundle = motion
            .key
            .split('#')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        // Owners the loading function itself gives the actor, and every other model the script
        // loads into that actor, consulted only when none of the former fit.
        let mut owners = BTreeMap::<usize, BTreeSet<String>>::new();
        let mut wider = BTreeMap::<usize, BTreeSet<String>>::new();
        let mut player = Vec::new();
        for (script, load) in report
            .script_loads
            .iter()
            .filter(|(_, l)| resolve(&l.path).contains(&bundle))
        {
            let call = if load.motion {
                "_LOAD_MOTION"
            } else {
                "_LOAD_CHARA"
            };
            let actor = match load.global {
                Some(slot) => format!("actor {} (global {slot})", load.actor),
                None => format!("actor {}", load.actor),
            };
            if !load.motion {
                // The pack's own profile builds or extends the actor; its own models are candidates.
                for &rig in by_path.get(&bundle).into_iter().flatten() {
                    owners.entry(rig).or_default().insert(format!(
                        "{script}@{}: {call} {actor} loads the pack itself",
                        load.instruction
                    ));
                }
            }
            if load.actor == 0 {
                player.push(format!("{script}@{}: {call} {actor}", load.instruction));
                continue;
            }
            // Models of other packs loaded into the same actor.
            let loads: Vec<_> = characters
                .get(&(script.as_str(), load.actor))
                .map_or(&[][..], Vec::as_slice)
                .iter()
                .filter(|c| !resolve(&c.path).contains(&bundle))
                .copied()
                .collect();
            let last = loads
                .iter()
                .filter(|c| c.function == load.function && c.instruction < load.instruction)
                .max_by_key(|c| c.instruction);
            for character in &loads {
                let primary = last.is_none_or(|l| std::ptr::eq(*l, *character));
                let map = if primary { &mut owners } else { &mut wider };
                for path in resolve(&character.path) {
                    for &rig in by_path.get(&path).into_iter().flatten() {
                        map.entry(rig).or_default().insert(format!(
                            "{script}@{}: {call} {actor} after _LOAD_CHARA {}@{}",
                            load.instruction, character.path, character.instruction
                        ));
                    }
                }
            }
        }
        for (pack, owner, evidence) in &CODE_LINKS {
            if published(pack) != bundle {
                continue;
            }
            match owner {
                CodeOwner::MainUnit => player.push((*evidence).to_owned()),
                CodeOwner::Pack(target) => {
                    for &rig in by_path.get(&published(target)).into_iter().flatten() {
                        owners
                            .entry(rig)
                            .or_default()
                            .insert((*evidence).to_owned());
                    }
                }
            }
        }
        if !player.is_empty() {
            for rig in fitting_rigs(motion, &report.rigs, &main_unit) {
                owners
                    .entry(rig)
                    .or_default()
                    .extend(player.iter().map(|e| format!("player unit: {e}")));
            }
        }
        let mut linked: BTreeSet<usize> = owners.keys().copied().collect();
        let mut fitting = fitting_rigs(motion, &report.rigs, &linked);
        if fitting.is_empty() && !wider.is_empty() {
            for (rig, evidence) in wider {
                owners.entry(rig).or_default().extend(evidence);
            }
            linked = owners.keys().copied().collect();
            fitting = fitting_rigs(motion, &report.rigs, &linked);
        }
        let rejected: Vec<String> = linked
            .difference(&fitting)
            .map(|&rig| format!("{} model {}", report.rigs[rig].path, report.rigs[rig].model))
            .collect();
        let mut reason = String::new();
        let (rigs, method) = if !fitting.is_empty() {
            (fitting, "linked")
        } else {
            let leaf = bundle.rsplit('/').next().unwrap_or(&bundle);
            let pack = format!("{}.chr", leaf.trim_end_matches(".gltf"));
            let mentions: Vec<&String> = report
                .script_literals
                .iter()
                .filter(|(_, literals)| {
                    literals.iter().any(|l| match l.contains('/') {
                        true => published(l) == bundle,
                        false => *l == pack,
                    })
                })
                .map(|(script, _)| script)
                .collect();
            // A motion nothing loads is placed after every other motion, from its loaded copies.
            let rigs = if mentions.is_empty() && linked.is_empty() {
                orphans.push(out.len());
                BTreeSet::new()
            } else {
                let all: BTreeSet<usize> = (0..report.rigs.len()).collect();
                fitting_rigs(motion, &report.rigs, &all)
            };
            let evidence = if !linked.is_empty() {
                "its script-loaded owners do not fit its skeleton".to_owned()
            } else if mentions.is_empty() {
                "no script or game code loads it".to_owned()
            } else {
                format!(
                    "named only outside constant _LOAD_MOTION calls in {} scripts, e.g. {}",
                    mentions.len(),
                    mentions[0]
                )
            };
            if !rigs.is_empty() {
                unresolved.push(format!(
                    "{}: {evidence}; placed by skeleton match into {} models",
                    motion.key,
                    rigs.len()
                ));
            }
            reason = evidence;
            (rigs, "heuristic")
        };
        placements.push(json!({
            "motion": motion.key,
            "method": method,
            "reason": reason,
            "targets": rigs.iter().map(|&r| json!({
                "model": report.rigs[r].path,
                "index": report.rigs[r].model,
                "evidence": owners.get(&r).cloned().unwrap_or_default(),
            })).collect::<Vec<_>>(),
            "rejected": rejected,
        }));
        out.push(rigs);
    }
    // A motion nothing loads goes where the game loads a copy of it (a pack with the same name),
    // when that copy's models fit; otherwise into the single model its skeleton fits best.
    for index in orphans {
        let motion = &report.externals[index];
        let bundle = motion
            .key
            .split('#')
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        let (copies, copy_names) =
            linked_copy_candidates(&report.externals, &placements, &out, &bundle);
        let mut rigs = fitting_rigs(motion, &report.rigs, &copies);
        let reason = if !rigs.is_empty() {
            format!(
                "no script or game code loads it; published where the game loads its copy {}",
                copy_names.join(", ")
            )
        } else {
            let all: BTreeSet<usize> = (0..report.rigs.len()).collect();
            let fitting = fitting_rigs(motion, &report.rigs, &all);
            let best = family_scores(motion, &report.rigs)
                .into_iter()
                .flat_map(|(score, members)| members.into_iter().map(move |m| (score, m)))
                .filter(|(_, m)| fitting.contains(m))
                .max_by(|a, b| {
                    (a.0, main_unit.contains(&a.1), std::cmp::Reverse(a.1))
                        .partial_cmp(&(b.0, main_unit.contains(&b.1), std::cmp::Reverse(b.1)))
                        .unwrap()
                });
            rigs = best
                .map(|(_, rig)| BTreeSet::from([rig]))
                .unwrap_or_default();
            "no script or game code loads it; published into the model its skeleton fits best"
                .to_owned()
        };
        unresolved.push(format!(
            "{}: {reason}; {}",
            motion.key,
            if rigs.is_empty() {
                "fits no model, not published".to_owned()
            } else {
                format!("placed by skeleton match into {} models", rigs.len())
            }
        ));
        placements[index]["reason"] = json!(reason);
        placements[index]["targets"] = json!(
            rigs.iter()
                .map(|&r| json!({"model": report.rigs[r].path, "index": report.rigs[r].model, "evidence": []}))
                .collect::<Vec<_>>()
        );
        out[index] = rigs;
    }
    report.placements = placements;
    report.unresolved.extend(unresolved);
    out
}

/// Publishes every external motion's actions into the models `place_externals` chooses.
fn publish_external(published: &Path, report: &mut ConversionReport) -> CoreResult<()> {
    let port = published.join("data");
    let mut work = BTreeMap::<String, Vec<(usize, usize)>>::new();
    let mut targets = vec![BTreeSet::<String>::new(); report.externals.len()];
    for (index, rigs) in place_externals(report).into_iter().enumerate() {
        for rig in rigs {
            work.entry(report.rigs[rig].path.clone())
                .or_default()
                .push((index, rig));
            targets[index].insert(report.rigs[rig].path.clone());
        }
    }
    for (path, list) in work {
        let gltf_path = port.join(&path);
        let sidecar_path = gltf_path.with_extension("game.json");
        let mut doc: Value = serde_json::from_slice(&fs::read(&gltf_path)?)?;
        let mut sidecar: Value = serde_json::from_slice(&fs::read(&sidecar_path)?)?;
        let mut temp = Gltf::new();
        temp.doc["nodes"] = doc["nodes"].clone();
        let mut prefixes = BTreeMap::<String, String>::new();
        let mut entries = BTreeMap::<usize, Value>::new();
        for (motion_index, rig_index) in list {
            let motion = &report.externals[motion_index];
            let rig = &report.rigs[rig_index];
            let bundle = motion.key.split('#').next().unwrap_or("");
            let taken: BTreeSet<String> = prefixes.values().cloned().collect();
            let prefix = prefixes
                .entry(bundle.to_owned())
                .or_insert_with(|| {
                    let base = leaf_stem(bundle);
                    let mut name = base.clone();
                    let mut suffix = 2;
                    while taken.contains(&name) {
                        name = format!("{base}_{suffix}");
                        suffix += 1;
                    }
                    name
                })
                .clone();
            temp.rotation_nodes = rig
                .helpers
                .iter()
                .map(|(&frame, &helper)| (rig.node_offset + frame, helper))
                .collect();
            let entry = entries.entry(motion_index).or_insert_with(|| {
                let actions: Vec<Value> = motion
                    .actions
                    .iter()
                    .map(|(name, start, end)| {
                        let auxiliary: Vec<Value> = motion
                            .channels
                            .iter()
                            .filter(|c| c["type"].as_i64().is_some_and(|t| t > 2))
                            .map(|c| super::actions::slice_aux(c, *start, *end))
                            .collect();
                        json!({"name":name,"duration":end - start,"channels":auxiliary})
                    })
                    .collect();
                json!({"external":motion.key,"actions":actions,"variants":[]})
            });
            let previous = entry["variants"].as_array().unwrap().len();
            let scene = doc["scenes"]
                [sidecar["models"][rig.model]["scene"].as_u64().unwrap_or(0) as usize]["name"]
                .as_str()
                .unwrap_or("")
                .to_owned();
            let mut clips = Vec::new();
            for (name, start, end) in &motion.actions {
                let clip = clip_name(&format!("{prefix}/{name}"), previous, &scene);
                clips.push(json!(animation_clip(
                    &mut temp,
                    &clip,
                    &motion.channels,
                    (*start, *end),
                    rig.node_offset,
                    false
                )?));
            }
            entry["variants"]
                .as_array_mut()
                .unwrap()
                .push(json!({"model":rig.model,"node_offset":rig.node_offset,"animations":clips}));
        }
        report.transforms.add(&temp.stats);
        doc["nodes"] = temp.doc["nodes"].clone();
        let added = temp.finish();
        let offsets: BTreeMap<&str, usize> = ["buffers", "bufferViews", "accessors", "animations"]
            .into_iter()
            .map(|key| (key, doc[key].as_array().map_or(0, Vec::len)))
            .collect();
        for key in ["buffers", "bufferViews", "accessors", "animations"] {
            for item in added[key].as_array().into_iter().flatten() {
                let mut value = item.clone();
                match key {
                    "bufferViews" => shift_field(&mut value, "buffer", offsets["buffers"]),
                    "accessors" => shift_field(&mut value, "bufferView", offsets["bufferViews"]),
                    "animations" => {
                        for sampler in value["samplers"].as_array_mut().unwrap() {
                            shift(&mut sampler["input"], offsets["accessors"]);
                            shift(&mut sampler["output"], offsets["accessors"]);
                        }
                    }
                    _ => {}
                }
                doc.as_object_mut()
                    .unwrap()
                    .entry(key)
                    .or_insert_with(|| json!([]))
                    .as_array_mut()
                    .unwrap()
                    .push(value);
            }
        }
        for entry in entries.into_values() {
            let mut entry = entry;
            for variant in entry["variants"].as_array_mut().unwrap() {
                for clip in variant["animations"].as_array_mut().unwrap() {
                    shift(clip, offsets["animations"]);
                }
            }
            prune_metadata(&mut entry);
            sidecar["resources"]["animations"]
                .as_array_mut()
                .ok_or_else(|| fail(format!("external motion target is not a character: {path}")))?
                .push(entry);
        }
        if let Some(buffer) = externalize_buffers(&port, &path, &mut doc)?
            && !report.files.contains(&buffer)
        {
            report.files.push(buffer);
        }
        save(&gltf_path, &doc)?;
        save(&sidecar_path, &sidecar)?;
    }
    for (motion, targets) in report.externals.iter().zip(targets) {
        let Some(sidecar) = &motion.sidecar else {
            continue;
        };
        let path = port.join(sidecar);
        let mut document: Value = serde_json::from_slice(&fs::read(&path)?)?;
        document["resources"]["animations"][motion.resource]["targets"] = json!(targets);
        save(&path, &document)?;
    }
    Ok(())
}

fn publish_external_memory(
    pairs: &mut BTreeMap<String, PackedPair>,
    report: &ConversionReport,
    work: &BTreeMap<String, Vec<(usize, usize)>>,
    targets: &[BTreeSet<String>],
) -> CoreResult<()> {
    for (path, list) in work {
        let Some(pair) = pairs.get_mut(path) else {
            continue;
        };
        let mut doc: Value = serde_json::from_slice(&pair.gltf)?;
        let mut sidecar: Value = serde_json::from_slice(&pair.metadata)?;
        let mut temp = Gltf::new();
        temp.doc["nodes"] = doc["nodes"].clone();
        let mut prefixes = BTreeMap::<String, String>::new();
        let mut entries = BTreeMap::<usize, Value>::new();
        for &(motion_index, rig_index) in list {
            let motion = &report.externals[motion_index];
            let rig = &report.rigs[rig_index];
            let bundle = motion.key.split('#').next().unwrap_or("");
            let taken: BTreeSet<String> = prefixes.values().cloned().collect();
            let prefix = prefixes
                .entry(bundle.to_owned())
                .or_insert_with(|| {
                    let base = leaf_stem(bundle);
                    let mut name = base.clone();
                    let mut suffix = 2;
                    while taken.contains(&name) {
                        name = format!("{base}_{suffix}");
                        suffix += 1;
                    }
                    name
                })
                .clone();
            temp.rotation_nodes = rig
                .helpers
                .iter()
                .map(|(&frame, &helper)| (rig.node_offset + frame, helper))
                .collect();
            let entry = entries.entry(motion_index).or_insert_with(|| {
                let actions: Vec<Value> = motion
                    .actions
                    .iter()
                    .map(|(name, start, end)| {
                        let auxiliary: Vec<Value> = motion
                            .channels
                            .iter()
                            .filter(|channel| channel["type"].as_i64().is_some_and(|kind| kind > 2))
                            .map(|channel| super::actions::slice_aux(channel, *start, *end))
                            .collect();
                        json!({"name":name,"duration":end-start,"channels":auxiliary})
                    })
                    .collect();
                json!({"external":motion.key,"actions":actions,"variants":[]})
            });
            let previous = entry["variants"].as_array().unwrap().len();
            let scene = doc["scenes"]
                [sidecar["models"][rig.model]["scene"].as_u64().unwrap_or(0) as usize]["name"]
                .as_str()
                .unwrap_or("")
                .to_owned();
            let mut clips = Vec::new();
            for (name, start, end) in &motion.actions {
                let clip = clip_name(&format!("{prefix}/{name}"), previous, &scene);
                clips.push(json!(animation_clip(
                    &mut temp,
                    &clip,
                    &motion.channels,
                    (*start, *end),
                    rig.node_offset,
                    false
                )?));
            }
            entry["variants"]
                .as_array_mut()
                .unwrap()
                .push(json!({"model":rig.model,"node_offset":rig.node_offset,"animations":clips}));
        }
        doc["nodes"] = temp.doc["nodes"].clone();
        let added = temp.finish();
        let offsets: BTreeMap<&str, usize> = ["buffers", "bufferViews", "accessors", "animations"]
            .into_iter()
            .map(|key| (key, doc[key].as_array().map_or(0, Vec::len)))
            .collect();
        for key in ["buffers", "bufferViews", "accessors", "animations"] {
            for item in added[key].as_array().into_iter().flatten() {
                let mut value = item.clone();
                match key {
                    "bufferViews" => shift_field(&mut value, "buffer", offsets["buffers"]),
                    "accessors" => shift_field(&mut value, "bufferView", offsets["bufferViews"]),
                    "animations" => {
                        for sampler in value["samplers"].as_array_mut().unwrap() {
                            shift(&mut sampler["input"], offsets["accessors"]);
                            shift(&mut sampler["output"], offsets["accessors"]);
                        }
                    }
                    _ => {}
                }
                doc.as_object_mut()
                    .unwrap()
                    .entry(key)
                    .or_insert_with(|| json!([]))
                    .as_array_mut()
                    .unwrap()
                    .push(value);
            }
        }
        for mut entry in entries.into_values() {
            for variant in entry["variants"].as_array_mut().unwrap() {
                for clip in variant["animations"].as_array_mut().unwrap() {
                    shift(clip, offsets["animations"]);
                }
            }
            prune_metadata(&mut entry);
            sidecar["resources"]["animations"]
                .as_array_mut()
                .ok_or_else(|| fail(format!("external motion target is not a character: {path}")))?
                .push(entry);
        }
        pair.buffer =
            externalize_buffers_memory_with_existing(path, &mut doc, pair.buffer.as_deref())?;
        pair.gltf = serde_json::to_vec(&doc)?;
        pair.metadata = serde_json::to_vec(&sidecar)?;
    }
    for (motion, targets) in report.externals.iter().zip(targets) {
        let Some(sidecar) = &motion.sidecar else {
            continue;
        };
        let path = format!(
            "{}.gltf",
            sidecar
                .strip_suffix(".game.json")
                .ok_or_else(|| fail(format!("invalid external sidecar path: {sidecar}")))?
        );
        let Some(pair) = pairs.get_mut(&path) else {
            continue;
        };
        let mut metadata: Value = serde_json::from_slice(&pair.metadata)?;
        metadata["resources"]["animations"][motion.resource]["targets"] = json!(targets);
        pair.metadata = serde_json::to_vec(&metadata)?;
    }
    Ok(())
}

fn submit_stream_pair(sink: &AssetSink, path: String, pair: PackedPair) -> CoreResult<()> {
    if let Some(buffer) = pair.buffer {
        sink.submit(
            buffer_path(&path),
            ArtifactBytes::Owned(buffer),
            Category::Model,
        )?;
    }
    let sidecar = Path::new(&path)
        .with_extension("game.json")
        .to_string_lossy()
        .replace('\\', "/");
    sink.submit(path, ArtifactBytes::Owned(pair.gltf), Category::Model)?;
    sink.submit(
        sidecar,
        ArtifactBytes::Owned(pair.metadata),
        Category::Model,
    )
}

fn collect(dir: &Path, files: &mut Vec<PathBuf>) -> CoreResult<()> {
    for e in fs::read_dir(dir)? {
        let p = e?.path();
        if p.is_dir() {
            collect(&p, files)?
        } else if p.is_file() {
            files.push(p)
        }
    }
    Ok(())
}

struct MemoryCardIcon {
    alias: String,
    path: String,
    image: String,
    gltf: Value,
    png: Vec<u8>,
}

struct ModelOutput {
    alias: String,
    artifact: NativeArtifact,
    catalog: Vec<(String, Value)>,
    stats: TransformStats,
}

/// Second-pass action for a resource that no model of its file converted.
enum Standalone {
    Skip,
    Done,
    Invalid,
    Motion(Option<ExternalMotion>),
    Artifact(NativeArtifact, Value),
}

/// Everything one source file contributes, computed without reading shared conversion state.
struct FileAnalysis {
    alias: String,
    icons: Vec<MemoryCardIcon>,
    script_loads: Vec<(String, super::native_scripts::ActorLoad)>,
    script_literals: Vec<(String, BTreeSet<String>)>,
    models: Vec<ModelOutput>,
    standalone: Vec<(String, Standalone)>,
    used_textures: BTreeSet<String>,
    resources: Vec<Resource>,
}

impl FileAnalysis {
    fn pack_artifacts(&mut self) -> CoreResult<()> {
        for model in &mut self.models {
            model.artifact.pack()?;
        }
        for (_, step) in &mut self.standalone {
            if let Standalone::Artifact(artifact, _) = step {
                artifact.pack()?;
            }
        }
        Ok(())
    }
}

/// Assigns unique flattened PNG names to every memory-card icon before parallel conversion.
fn icon_texture_paths(
    inventory: &super::inventory::Inventory,
    png_names: &BTreeSet<String>,
) -> CoreResult<BTreeMap<String, String>> {
    let mut used: BTreeSet<_> = png_names
        .iter()
        .map(|name| name.to_ascii_lowercase())
        .collect();
    let mut paths = BTreeMap::new();
    for alias in inventory
        .names()
        .filter(|alias| extension(alias).eq_ignore_ascii_case("ico"))
    {
        let candidate = super::textures::document_path(
            &Path::new(alias).with_extension("img").to_string_lossy(),
        )?
        .replace(".json", ".png");
        paths.insert(
            alias.to_owned(),
            super::textures::reserve_path(candidate, alias, &mut used),
        );
    }
    Ok(paths)
}

fn analyze_file(
    source: &super::archive::SourceTree,
    alias: &str,
    all_textures: &[Texture],
    icon_paths: &BTreeMap<String, String>,
    phase: &Phase,
    embedded_gameplay: bool,
) -> CoreResult<Option<FileAnalysis>> {
    phase.item(alias);
    if source.len(alias)? > 128 << 20 {
        return Ok(None);
    }
    let mut resources = Vec::new();
    discover(&source.read(alias)?, alias, &mut resources, 0)?;
    let mut icons = Vec::new();
    for icon in resources
        .iter()
        .filter(|resource| extension(&resource.alias).eq_ignore_ascii_case("ico"))
    {
        let path = super::configs::native_path(&icon.alias, "gltf");
        let image = icon_paths
            .get(&icon.alias)
            .ok_or_else(|| fail(format!("unplanned icon image: {}", icon.alias)))?
            .clone();
        let (gltf, png) = memory_card_icon(&icon.data, &relative_uri(&path, &image))?;
        icons.push(MemoryCardIcon {
            alias: icon.alias.clone(),
            path,
            image,
            gltf,
            png,
        });
    }
    if matches!(extension(alias), "mds" | "mdt") {
        for ext in ["mot", "wgt", "bbp"] {
            let sibling_alias = Path::new(alias)
                .with_extension(ext)
                .to_string_lossy()
                .replace('\\', "/");
            if source.contains(&sibling_alias) {
                discover(
                    &source.read(&sibling_alias)?,
                    &sibling_alias,
                    &mut resources,
                    0,
                )?;
            }
        }
    }
    let mut script_loads = Vec::new();
    let mut script_literals = Vec::new();
    for script in resources.iter().filter(|r| {
        extension(&r.alias).eq_ignore_ascii_case("stb")
            && super::native_scripts::is_valid_script(&r.data)
    }) {
        for load in super::native_scripts::actor_loads(&script.data)? {
            script_loads.push((script.alias.clone(), load));
        }
        script_literals.push((
            script.alias.clone(),
            super::native_scripts::string_literals(&script.data)?,
        ));
    }
    let links = script_links(&resources);
    let keyed = if embedded_gameplay {
        let mut actions = super::actions::ProfileActions::default();
        for character in resources
            .iter()
            .filter(|resource| is_character(&resource.alias))
        {
            if let Some(plan) = super::configs::profile_actions_for(&character.alias)? {
                actions.motions.extend(plan.actions.motions.clone());
                actions.unbound.extend(plan.actions.unbound.clone());
                actions.order.extend(plan.actions.order.clone());
            } else if resources.iter().any(|resource| {
                extension(&resource.alias).eq_ignore_ascii_case("cfg")
                    && character_parent(&resource.alias) == Some(character.alias.as_str())
            }) {
                return Err(fail(format!(
                    "missing checked-in gameplay clip plan for {}",
                    character.alias
                )));
            }
        }
        actions
    } else {
        let mut profiles = Vec::new();
        for resource in resources.iter().filter(|resource| {
            extension(&resource.alias) == "cfg" && character_parent(&resource.alias).is_some()
        }) {
            profiles.push((
                resource.alias.clone(),
                super::configs::parse(&resource.data)?,
            ));
        }
        super::actions::key_actions(&profiles)?
    };
    let mut parsed = BTreeMap::new();
    let mut poses = BTreeMap::new();
    for r in &resources {
        match extension(&r.alias).to_ascii_lowercase().as_str() {
            "mot" | "wgt" => {
                parsed.insert(r.alias.clone(), channels(r)?);
            }
            "bbp" => {
                poses.insert(r.alias.clone(), bind_pose(r)?);
            }
            _ => {}
        }
    }
    let mut used_textures = BTreeSet::new();
    let mut models = Vec::new();
    for r in &resources {
        if !r.data.starts_with(b"MDS\0") && !r.data.starts_with(b"MDT\0") {
            continue;
        }
        let mut gltf = Gltf::new();
        let mut frames = Vec::new();
        let mut mdts = Vec::new();
        if r.data.starts_with(b"MDS\0") {
            scene_extent(&r.data)?;
            let mut at = u32le(&r.data, 12)? as usize;
            for id in 0..u32le(&r.data, 8)? as usize {
                let matrix = floats(&r.data, at + 48, 16)?;
                let mdt = u32le(&r.data, at + 40)? as usize;
                frames.push(json!({"id":id,"name":name(bytes(&r.data,at+8,32)?),"parent":i32le(&r.data,at+44)?,"local_matrix":matrix,"mdt":if mdt>0{Some(mdts.len())}else{None},"reserved_u32":[u32le(&r.data,at)?]}));
                if mdt > 0 {
                    mdts.push((id, mdt))
                }
                at += u32le(&r.data, at + 4)? as usize;
            }
        } else {
            frames.push(json!({"id":0,"name":r.alias,"parent":-1,"local_matrix":identity(),"mdt":0,"reserved_u32":[0]}));
            mdts.push((0, 0));
        }
        let mut roots = Vec::new();
        for f in &frames {
            let m: Vec<f32> = f["local_matrix"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_f64().unwrap() as f32)
                .collect();
            let mut node = json!({"matrix":m});
            node["name"] = f["name"].clone();
            gltf.doc["nodes"].as_array_mut().unwrap().push(node);
        }
        let mut world = vec![identity(); frames.len()];
        for (i, f) in frames.iter().enumerate() {
            let p = f["parent"].as_i64().unwrap();
            let m: Vec<f32> = f["local_matrix"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_f64().unwrap() as f32)
                .collect();
            if p < 0 {
                roots.push(i);
                world[i] = m;
            } else {
                if p as usize >= i {
                    return Err(fail(format!(
                        "MDS parent is not earlier than child: {}",
                        r.alias
                    )));
                }
                world[i] = multiply(&world[p as usize], &m);
                let node = &mut gltf.doc["nodes"][p as usize];
                if node["children"].is_null() {
                    node["children"] = json!([])
                }
                node["children"].as_array_mut().unwrap().push(json!(i));
            }
        }
        gltf.doc["scenes"][0]["nodes"] = json!(roots);
        let weights_alias = format!("{}.wgt", stem(&r.alias));
        let weight_channels = parsed.get(&weights_alias).cloned().unwrap_or_default();
        let pose_alias = format!("{}.bbp", stem(&r.alias));
        let selected_pose = poses.get(&pose_alias).or_else(|| {
            poses
                .iter()
                .find(|(alias, _)| links.get(&r.alias).is_some_and(|l| l.contains(*alias)))
                .map(|(_, p)| p)
        });
        let bind_world = if let Some(pose) = selected_pose.filter(|_| !weight_channels.is_empty()) {
            let matrices = pose["matrices"].as_array().unwrap();
            for channel in weight_channels
                .iter()
                .filter(|c| c["keys"].as_array().is_some_and(|k| !k.is_empty()))
            {
                for key in ["frame", "target"] {
                    if channel[key]
                        .as_u64()
                        .is_none_or(|n| n >= matrices.len() as u64)
                    {
                        return Err(fail(format!(
                            "skin references absent bind matrix: {}",
                            r.alias
                        )));
                    }
                }
            }
            let mut bind = world.clone();
            for (i, m) in matrices.iter().take(bind.len()).enumerate() {
                bind[i] = m
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|v| v.as_f64().unwrap() as f32)
                    .collect();
            }
            bind
        } else {
            world.clone()
        };
        let mut meshes = Vec::new();
        for &(frame, at) in &mdts {
            meshes.push(mesh(
                &mut gltf,
                &r.data[at..],
                frame,
                &r.alias,
                all_textures,
                &mut used_textures,
                &weight_channels,
                &bind_world,
            )?)
        }
        let linked: Vec<(&String, &Vec<Value>)> = parsed
            .iter()
            .filter(|(alias, cs)| {
                extension(alias) == "mot"
                    && compatible_motion(cs, frames.len())
                    && (stem(alias) == stem(&r.alias)
                        || links.get(&r.alias).is_some_and(|l| l.contains(*alias)))
            })
            .collect();
        let motion_channels: Vec<Value> = linked
            .iter()
            .flat_map(|(_, cs)| cs.iter().cloned())
            .collect();
        prepare_morphs(&mut gltf, &meshes, &motion_channels)?;
        let motion_lists: Vec<&[Value]> = linked.iter().map(|(_, cs)| cs.as_slice()).collect();
        prepare_transform_nodes(&mut gltf, &motion_lists)?;
        for (&frame, &helper) in &gltf.rotation_nodes {
            frames[frame]["rotation_node"] = json!(helper);
        }
        let mut animation_aliases = Vec::new();
        for (alias, cs) in &linked {
            let list = keyed.motions.get(*alias).cloned().unwrap_or_else(|| {
                vec![KeyAction {
                    game: Value::Null,
                    start: 0,
                    end: motion_extent(cs),
                }]
            });
            let mut clips = Vec::new();
            for action in list {
                let name = format!("{}#{}", leaf_stem(alias), clips.len());
                let id = animation_clip(&mut gltf, &name, cs, (action.start, action.end), 0, true)?;
                clips.push(
                    json!({"game":action.game,"start":action.start,"end":action.end,"index":id}),
                );
            }
            animation_aliases.push(json!({"alias":alias,"clips":clips}));
        }
        let stats = gltf.stats.clone();
        let bind_poses: Vec<_> = poses
            .iter()
            .filter(|(alias, _)| {
                *alias == &pose_alias || links.get(&r.alias).is_some_and(|l| l.contains(*alias))
            })
            .map(|(_, v)| v.clone())
            .collect();
        let weights: Vec<_> = if weight_channels.is_empty() {
            Vec::new()
        } else {
            vec![json!({"alias":weights_alias,"channels":weight_channels})]
        };
        let gltf_path = output_path(&r.alias, "gltf");
        let metadata_path = output_path(&r.alias, "metadata.json");
        let metadata = json!({"schema_version":1,"source":r.alias,"frames":frames,"meshes":meshes,"motion_channels":motion_channels,"bind_poses":bind_poses,"weights":weights,"frames_per_second":30});
        let mut catalog = Vec::new();
        for a in &animation_aliases {
            let alias = a["alias"].as_str().unwrap();
            catalog.push((
                alias.to_string(),
                json!({"alias":alias,"kind":"motion","gltf":gltf_path,"metadata":metadata_path}),
            ));
        }
        for pose in &bind_poses {
            let alias = pose["alias"].as_str().unwrap();
            catalog.push((
                alias.to_string(),
                json!({"alias":alias,"kind":"bind_pose","gltf":gltf_path,"metadata":metadata_path}),
            ));
        }
        if parsed.contains_key(&weights_alias) {
            catalog.push((weights_alias.clone(), json!({"alias":weights_alias,"kind":"weights","gltf":gltf_path,"metadata":metadata_path})));
        }
        models.push(ModelOutput {
            alias: r.alias.clone(),
            artifact: NativeArtifact {
                gltf: gltf.finish(),
                metadata,
                animations: animation_aliases,
                packed: None,
            },
            catalog,
            stats,
        });
    }
    let known: BTreeSet<String> = models
        .iter()
        .flat_map(|model| model.catalog.iter().map(|(alias, _)| alias.clone()))
        .collect();
    let mut standalone = Vec::new();
    for r in &resources {
        let ext = extension(&r.alias);
        let step = if matches!(ext, "cfg" | "clo" | "chr" | "chr_" | "img" | "tm2" | "stb") {
            Standalone::Skip
        } else if !matches!(ext, "mot" | "wgt" | "bbp") {
            Standalone::Invalid
        } else if known.contains(&r.alias) {
            Standalone::Done
        } else if ext == "mot" {
            let external = if character_parent(&r.alias).is_none() {
                // A motion outside any character is published only into the models it drives.
                let channels = parsed.get(&r.alias).cloned().unwrap_or_default();
                let name = super::actions::stem_name(&leaf_stem(&r.alias));
                Some(ExternalMotion {
                    key: format!("{}#0", super::configs::native_path(&r.alias, "gltf")),
                    sidecar: None,
                    resource: 0,
                    actions: vec![(name, 0, motion_extent(&channels))],
                    channels,
                })
            } else {
                None
            };
            Standalone::Motion(external)
        } else {
            let mut gltf = Gltf::new();
            let cs = parsed.get(&r.alias).cloned().unwrap_or_default();
            let count = cs
                .iter()
                .filter_map(|c| c["frame"].as_u64())
                .max()
                .map_or_else(
                    || {
                        poses
                            .get(&r.alias)
                            .and_then(|p| p["matrices"].as_array())
                            .map_or(0, |a| a.len())
                    },
                    |n| n as usize + 1,
                );
            for i in 0..count {
                let mut node = json!({"name":format!("frame_{i}")});
                if let Some(pose) = poses.get(&r.alias) {
                    node["matrix"] = pose["matrices"][i].clone();
                }
                gltf.doc["nodes"].as_array_mut().unwrap().push(node);
            }
            gltf.doc["scenes"][0]["nodes"] = json!((0..count).collect::<Vec<_>>());
            let gp = output_path(&r.alias, "gltf");
            let mp = output_path(&r.alias, "metadata.json");
            let metadata = json!({"schema_version":1,"source":r.alias,"frames":[],"meshes":[],"motion_channels":[],"weights":if ext=="wgt"{vec![json!({"alias":r.alias,"channels":cs})]}else{Vec::new()},"bind_poses":poses.get(&r.alias).cloned().into_iter().collect::<Vec<_>>(),"frames_per_second":30});
            let entry = json!({"alias":r.alias,"kind":if ext=="wgt"{"weights"}else{"bind_pose"},"gltf":gp,"metadata":mp});
            Standalone::Artifact(
                NativeArtifact {
                    gltf: gltf.finish(),
                    metadata,
                    animations: Vec::new(),
                    packed: None,
                },
                entry,
            )
        };
        standalone.push((r.alias.clone(), step));
    }
    Ok(Some(FileAnalysis {
        alias: alias.to_owned(),
        icons,
        script_loads,
        script_literals,
        models,
        standalone,
        used_textures,
        resources,
    }))
}

/// Native publication of one committed file, with the report entries its commit produced.
struct NativeJob {
    resources: Vec<Resource>,
    artifacts: BTreeMap<String, NativeArtifact>,
    icons: Vec<MemoryCardIcon>,
    files: Vec<String>,
    externals: Vec<ExternalMotion>,
}

impl NativeJob {
    fn estimated_expanded_bytes(&self) -> usize {
        let source = self
            .resources
            .iter()
            .map(|resource| resource.data.len())
            .sum::<usize>()
            .saturating_mul(4);
        let decoded = self
            .artifacts
            .values()
            .map(|artifact| {
                artifact
                    .packed
                    .as_ref()
                    .map_or(0, |packed| packed.decoded_len)
            })
            .sum::<usize>()
            .saturating_mul(5);
        source.saturating_add(decoded).max(1 << 20)
    }
}

pub(crate) const MODEL_DEFERRED_WORK_MULTIPLIER: u64 = 8;
pub(crate) const MODEL_PREVIEW_WORK_DIVISOR: u64 = 3;

fn apportion_model_work(weights: &[u128], budget: u64) -> Vec<u64> {
    if weights.is_empty() {
        return Vec::new();
    }
    let minimum = u64::from(budget >= weights.len() as u64);
    let variable_budget = budget - minimum * weights.len() as u64;
    let raw_total: u128 = weights.iter().sum();
    let total = if raw_total == 0 {
        weights.len() as u128
    } else {
        raw_total
    };
    let mut previous = 0u128;
    weights
        .iter()
        .map(|&raw_weight| {
            let weight = if raw_total == 0 { 1 } else { raw_weight };
            let next = previous + weight;
            let work = minimum
                + ((next * variable_budget as u128) / total
                    - (previous * variable_budget as u128) / total) as u64;
            previous = next;
            work
        })
        .collect()
}

fn external_motion_work(motion: &ExternalMotion) -> u128 {
    let channels = motion.channels.len() as u128;
    let keys = motion
        .channels
        .iter()
        .map(|channel| channel["keys"].as_array().map_or(0, Vec::len) as u128)
        .sum::<u128>();
    let scales = motion
        .channels
        .iter()
        .filter(|channel| channel["type"].as_i64() == Some(1))
        .count() as u128;
    motion.actions.iter().fold(0u128, |work, (_, start, end)| {
        let frames = end.saturating_sub(*start).saturating_add(1) as u128;
        work.saturating_add(keys + channels + frames.saturating_mul(scales))
    })
}

fn final_model_weights(preview_nanos: &[u128], external_ops: &[u128]) -> Vec<u128> {
    let preview_total: u128 = preview_nanos.iter().sum::<u128>().max(1);
    let external_total: u128 = external_ops.iter().sum();
    preview_nanos
        .iter()
        .zip(external_ops)
        .map(|(&preview, &external)| {
            let external_weight = external
                .saturating_mul(preview_total)
                .saturating_mul(2)
                .checked_div(external_total)
                .unwrap_or(0);
            preview.saturating_add(external_weight).max(1)
        })
        .collect()
}

fn commit_file(
    published: &Path,
    analysis: FileAnalysis,
    converted: &mut BTreeSet<String>,
    catalog_resources: &mut BTreeMap<String, Value>,
    used_textures: &mut BTreeSet<String>,
    report: &mut ConversionReport,
    stream: bool,
) -> CoreResult<Option<NativeJob>> {
    if converted.contains(&analysis.alias) {
        return Ok(None);
    }
    let mut files = Vec::new();
    let mut externals = Vec::new();
    let mut icons = Vec::new();
    for icon in analysis.icons {
        if !stream {
            let image_path = published.join("data").join(&icon.image);
            if image_path.exists() && fs::read(&image_path)? != icon.png {
                return Err(fail(format!(
                    "memory card icon image collides: {}",
                    icon.image
                )));
            }
            save(&published.join("data").join(&icon.path), &icon.gltf)?;
            fs::create_dir_all(image_path.parent().unwrap())?;
            fs::write(&image_path, &icon.png)?;
        }
        report.referenced_pngs.insert(icon.image.clone());
        files.push(icon.image.clone());
        files.push(icon.path.clone());
        converted.insert(icon.alias.clone());
        if stream {
            icons.push(icon);
        }
    }
    used_textures.extend(analysis.used_textures);
    report.script_loads.extend(analysis.script_loads);
    report.script_literals.extend(analysis.script_literals);
    let mut artifacts = BTreeMap::new();
    for model in analysis.models {
        report.transforms.add(&model.stats);
        converted.insert(model.alias.clone());
        catalog_resources.extend(model.catalog);
        artifacts.insert(model.alias, model.artifact);
    }
    for (alias, step) in analysis.standalone {
        if converted.contains(&alias) {
            continue;
        }
        match step {
            Standalone::Skip => continue,
            Standalone::Invalid => return Err(fail(format!("invalid model resource {alias}"))),
            Standalone::Done => {}
            Standalone::Motion(external) => {
                if !catalog_resources.contains_key(&alias) {
                    externals.extend(external);
                }
            }
            Standalone::Artifact(artifact, entry) => {
                if !catalog_resources.contains_key(&alias) {
                    artifacts.insert(alias.clone(), artifact);
                    catalog_resources.insert(alias.clone(), entry);
                }
            }
        }
        converted.insert(alias);
    }
    converted.extend(native_claims(&analysis.resources));
    Ok(Some(NativeJob {
        resources: analysis.resources,
        artifacts,
        icons,
        files,
        externals,
    }))
}

/// Publishes committed files in parallel, merging their report entries in file order.
///
/// Files whose outputs overlap are published again one after another, so the later file's
/// output is the one kept.
fn publish_jobs(
    published: &Path,
    jobs: Vec<NativeJob>,
    documents: &[super::textures::ConvertedTextureDocument],
    report: &mut ConversionReport,
) -> CoreResult<()> {
    let publish = |job: &NativeJob| -> CoreResult<ConversionReport> {
        let mut local = ConversionReport::default();
        publish_native(
            published,
            &job.resources,
            &job.artifacts,
            documents,
            &mut local,
        )?;
        Ok(local)
    };
    let locals = jobs
        .par_iter()
        .map(publish)
        .collect::<Vec<_>>()
        .into_iter()
        .collect::<CoreResult<Vec<_>>>()?;
    let mut writers = BTreeMap::<&str, usize>::new();
    for local in &locals {
        for file in &local.files {
            *writers.entry(file).or_default() += 1;
        }
    }
    for (job, local) in jobs.iter().zip(&locals) {
        if local.files.iter().any(|file| writers[file.as_str()] > 1) {
            publish(job)?;
        }
    }
    for (job, local) in jobs.into_iter().zip(locals) {
        report.files.extend(job.files);
        report.files.extend(local.files);
        report.externals.extend(job.externals);
        report.externals.extend(local.externals);
        report.rigs.extend(local.rigs);
        report.referenced_pngs.extend(local.referenced_pngs);
        report
            .consumed_texture_sources
            .extend(local.consumed_texture_sources);
    }
    Ok(())
}

/// An in-memory model pass whose resource analysis can run beside every other asset family.
pub struct ModelPlan {
    pub consumed: BTreeSet<String>,
    pub embedded_texture_sources: BTreeSet<String>,
    pub deferred_work: u64,
    jobs: Vec<Job>,
    analyses: Arc<Mutex<Vec<Option<Option<FileAnalysis>>>>>,
    documents: Arc<Vec<super::textures::ConvertedTextureDocument>>,
    png_names: Arc<BTreeSet<String>>,
}

impl ModelPlan {
    pub fn into_initial_jobs(&mut self) -> Vec<Job> {
        std::mem::take(&mut self.jobs)
    }

    /// Resolves model ownership and external-motion placement before final ZIP entries are sent.
    pub fn finish_jobs(
        self,
        sink: &AssetSink,
        on_preview: &(dyn Fn(u64, &str) + Sync),
    ) -> CoreResult<Vec<Job>> {
        let mut report = ConversionReport::default();
        let mut converted = BTreeSet::new();
        let mut catalog_resources = BTreeMap::new();
        let mut used_textures = BTreeSet::new();
        let mut native_jobs = Vec::new();
        let analyses = Arc::try_unwrap(self.analyses)
            .map_err(|_| fail("model analysis jobs are still running"))?
            .into_inner()
            .map_err(|_| fail("model analysis lock poisoned"))?;
        for analysis in analyses {
            let analysis = analysis.ok_or_else(|| fail("model analysis job did not complete"))?;
            if let Some(analysis) = analysis
                && let Some(job) = commit_file(
                    Path::new("published"),
                    analysis,
                    &mut converted,
                    &mut catalog_resources,
                    &mut used_textures,
                    &mut report,
                    true,
                )?
            {
                native_jobs.push(job);
            }
        }
        let documents = self.documents;
        let png_names = self.png_names;
        let weights: Vec<u128> = native_jobs
            .iter()
            .map(|job| job.estimated_expanded_bytes() as u128)
            .collect();
        let preview_budget = if weights.is_empty() {
            0
        } else {
            self.deferred_work / MODEL_PREVIEW_WORK_DIVISOR
        };
        let preview_work = apportion_model_work(&weights, preview_budget);
        // This pass records ownership and rigs without retaining generated glTF buffers.
        // Each final job regenerates only its own pairs under the sink's memory admission.
        let previews: Vec<CoreResult<(BTreeSet<String>, ConversionReport, std::time::Duration)>> =
            native_jobs
                .par_iter()
                .enumerate()
                .map(|(index, job)| {
                    let estimate = job.estimated_expanded_bytes();
                    let result = sink.with_admission(estimate, || {
                        let started = std::time::Instant::now();
                        let mut pairs = BTreeMap::new();
                        let mut local = ConversionReport::default();
                        let mut stream = StreamPairs {
                            pairs: &mut pairs,
                            png_names: &png_names,
                            gameplay: None,
                            preview: true,
                        };
                        publish_native_impl(
                            Path::new("published"),
                            &job.resources,
                            &job.artifacts,
                            &documents,
                            &mut local,
                            Some(&mut stream),
                        )?;
                        let elapsed = started.elapsed();
                        if elapsed.as_secs() >= 1
                            && std::env::var_os("DC2DATA_PROFILE_JOBS").is_some()
                        {
                            let label = job
                                .resources
                                .first()
                                .map_or("model-job", |resource| resource.alias.as_str());
                            eprintln!("dc2data job preview {label}: {:.3}s", elapsed.as_secs_f64());
                        }
                        Ok((pairs.into_keys().collect(), local, elapsed))
                    })?;
                    let work = preview_work[index];
                    sink.complete(work)?;
                    let label = job
                        .resources
                        .first()
                        .map_or("", |resource| resource.alias.as_str());
                    on_preview(work, label);
                    Ok(result)
                })
                .collect();
        let mut owners = BTreeMap::<String, usize>::new();
        let mut icon_names = BTreeSet::new();
        let mut preview_nanos = Vec::with_capacity(native_jobs.len());
        for (index, (job, result)) in native_jobs.iter().zip(previews).enumerate() {
            let (names, local, elapsed) = result?;
            preview_nanos.push(elapsed.as_nanos().max(1));
            for name in names {
                owners.insert(name, index);
            }
            report.rigs.extend(local.rigs);
            report.externals.extend(job.externals.iter().cloned());
            report.externals.extend(local.externals);
            report.referenced_pngs.extend(local.referenced_pngs);
            report
                .consumed_texture_sources
                .extend(local.consumed_texture_sources);
            for icon in &job.icons {
                if !icon_names.insert(icon.path.clone()) {
                    return Err(fail("duplicate memory-card icon output"));
                }
            }
        }
        // A missing target is still an error; partial final jobs may omit other owners.
        let selected = place_externals(&mut report);
        let mut external_work = BTreeMap::<String, Vec<(usize, usize)>>::new();
        let mut external_targets = vec![BTreeSet::<String>::new(); report.externals.len()];
        for (motion_index, (motion, rigs)) in report.externals.iter().zip(selected).enumerate() {
            for rig in rigs {
                let path = &report.rigs[rig].path;
                if !owners.contains_key(path) {
                    return Err(fail(format!(
                        "external motion target missing: {} ({})",
                        path, motion.key
                    )));
                }
                external_work
                    .entry(path.clone())
                    .or_default()
                    .push((motion_index, rig));
                external_targets[motion_index].insert(path.clone());
            }
            if let Some(sidecar) = &motion.sidecar {
                let path = format!(
                    "{}.gltf",
                    sidecar
                        .strip_suffix(".game.json")
                        .ok_or_else(|| fail(format!("invalid external sidecar path: {sidecar}")))?
                );
                if !owners.contains_key(&path) {
                    return Err(fail(format!("external sidecar missing: {sidecar}")));
                }
            }
        }
        let owners = Arc::new(owners);
        let report = Arc::new(report);
        let mut external_ops = vec![0u128; native_jobs.len()];
        for (path, placements) in &external_work {
            let Some(&owner) = owners.get(path) else {
                continue;
            };
            for &(motion, _) in placements {
                external_ops[owner] = external_ops[owner]
                    .saturating_add(external_motion_work(&report.externals[motion]));
            }
        }
        let external_work = Arc::new(external_work);
        let external_targets = Arc::new(external_targets);
        let count = native_jobs.len();
        let final_budget = self.deferred_work - preview_budget;
        let final_weights = final_model_weights(&preview_nanos, &external_ops);
        let final_work = apportion_model_work(&final_weights, final_budget);
        let mut jobs = Vec::with_capacity(count.max(1));
        for (index, job) in native_jobs.into_iter().enumerate() {
            let work = final_work[index];
            let estimate = job.estimated_expanded_bytes();
            let documents = documents.clone();
            let png_names = png_names.clone();
            let report = report.clone();
            let owners = owners.clone();
            let external_work = external_work.clone();
            let external_targets = external_targets.clone();
            let label = job.resources.first().map_or_else(
                || format!("model-job-{index}"),
                |resource| resource.alias.clone(),
            );
            jobs.push(Job {
                category: Category::Model,
                label,
                work,
                estimated_bytes: estimate,
                run: Box::new(move |sink| {
                    let mut pairs = BTreeMap::new();
                    let mut local = ConversionReport::default();
                    let mut stream = StreamPairs {
                        pairs: &mut pairs,
                        png_names: &png_names,
                        gameplay: None,
                        preview: false,
                    };
                    publish_native_impl(
                        Path::new("published"),
                        &job.resources,
                        &job.artifacts,
                        &documents,
                        &mut local,
                        Some(&mut stream),
                    )?;
                    let mut pairs = pairs
                        .into_iter()
                        .map(|(name, pair)| Ok((name, pair.compact()?)))
                        .collect::<CoreResult<BTreeMap<_, _>>>()?;
                    publish_external_memory(
                        &mut pairs,
                        &report,
                        &external_work,
                        &external_targets,
                    )?;
                    for (name, pair) in pairs {
                        if owners.get(&name) == Some(&index) {
                            submit_stream_pair(sink, name, pair)?;
                        }
                    }
                    for icon in job.icons {
                        sink.submit(
                            icon.path,
                            ArtifactBytes::Owned(serde_json::to_vec(&icon.gltf)?),
                            Category::Model,
                        )?;
                        sink.submit(icon.image, ArtifactBytes::Owned(icon.png), Category::Model)?;
                    }
                    Ok(())
                }),
            });
        }
        if jobs.is_empty() && self.deferred_work > 0 {
            jobs.push(Job {
                category: Category::Model,
                label: "models-finalize".into(),
                work: self.deferred_work,
                estimated_bytes: 1,
                run: Box::new(|_| Ok(())),
            });
        }
        Ok(jobs)
    }
}

fn collect_character_gameplay(
    source: &super::archive::SourceTree,
) -> CoreResult<BTreeMap<String, Value>> {
    let inventory = super::inventory::Inventory::build(source)?;
    let texture_plan = super::textures::plan_source(&inventory)?;
    let documents = texture_plan.documents;
    let png_names = texture_plan.png_names;
    let all_textures = textures(Path::new("published"), &documents)?;
    let icon_paths = icon_texture_paths(&inventory, &png_names)?;
    let mut character_roots = BTreeSet::new();
    for alias in inventory.names().filter(|alias| is_character(alias)) {
        let mut ancestor = alias;
        loop {
            if source.contains(ancestor) {
                character_roots.insert(ancestor.to_owned());
                break;
            }
            let Some((parent, _)) = ancestor.rsplit_once('/') else {
                break;
            };
            ancestor = parent;
        }
    }
    let mut files: Vec<&str> = source
        .names()
        .filter(|name| character_roots.contains(*name))
        .collect();
    files.sort_by_key(|alias| (!matches!(extension(alias), "mds" | "mdt"), *alias));
    let mut report = ConversionReport::default();
    let mut converted = BTreeSet::new();
    let mut catalog_resources = BTreeMap::new();
    let mut used_textures = BTreeSet::new();
    let mut gameplay = BTreeMap::new();
    for alias in files {
        let analysis = analyze_file(
            source,
            alias,
            &all_textures,
            &icon_paths,
            &Phase::noop(),
            false,
        )
        .map_err(|error| fail(format!("analyze gameplay source {alias}: {error}")))?;
        let Some(analysis) = analysis else { continue };
        let Some(job) = commit_file(
            Path::new("published"),
            analysis,
            &mut converted,
            &mut catalog_resources,
            &mut used_textures,
            &mut report,
            true,
        )
        .map_err(|error| fail(format!("commit gameplay source {alias}: {error}")))?
        else {
            continue;
        };
        if !job
            .resources
            .iter()
            .any(|resource| is_character(&resource.alias))
        {
            continue;
        }
        let mut pairs = BTreeMap::new();
        let mut local = ConversionReport::default();
        let mut stream = StreamPairs {
            pairs: &mut pairs,
            png_names: &png_names,
            gameplay: Some(&mut gameplay),
            preview: false,
        };
        publish_native_impl(
            Path::new("published"),
            &job.resources,
            &job.artifacts,
            &documents,
            &mut local,
            Some(&mut stream),
        )
        .map_err(|error| fail(format!("render gameplay source {alias}: {error}")))?;
    }
    Ok(gameplay)
}

/// Refreshes loose, human-editable gameplay documents directly from one indexed ISO.
pub fn export_character_gameplay(
    source: &super::archive::SourceTree,
    destination: &Path,
) -> CoreResult<usize> {
    let documents = collect_character_gameplay(source)?;
    for (name, document) in &documents {
        let path = destination.join(name);
        let parent = path
            .parent()
            .ok_or_else(|| fail("gameplay path has no parent"))?;
        fs::create_dir_all(parent).map_err(|error| {
            fail(format!(
                "create gameplay directory {}: {error}",
                parent.display()
            ))
        })?;
        let mut bytes = serde_json::to_vec_pretty(document)?;
        bytes.push(b'\n');
        fs::write(&path, bytes).map_err(|error| {
            fail(format!(
                "write gameplay document {}: {error}",
                path.display()
            ))
        })?;
    }
    Ok(documents.len())
}

/// Compares loose gameplay semantics against the retail source without staging assets.
pub fn audit_character_gameplay(
    source: &super::archive::SourceTree,
    destination: &Path,
) -> CoreResult<usize> {
    let documents = collect_character_gameplay(source)?;
    for (name, expected) in &documents {
        let path = destination.join(name);
        let actual: Value = serde_json::from_slice(&fs::read(&path).map_err(|error| {
            fail(format!(
                "missing gameplay document {}: {error}",
                path.display()
            ))
        })?)?;
        // The exported document is the gameplay contract. Normalize the expected
        // value through that representation before comparing parsed JSON numbers.
        let exported: Value = serde_json::from_slice(&serde_json::to_vec(expected)?)?;
        if actual != exported {
            return Err(fail(format!(
                "gameplay document differs from retail: {}",
                path.display()
            )));
        }
    }
    Ok(documents.len())
}

/// Plans model source ownership and fixed work before the shared worker pool starts.
pub fn plan_source(
    source: Arc<super::archive::SourceTree>,
    inventory: &super::inventory::Inventory,
    documents: Arc<Vec<super::textures::ConvertedTextureDocument>>,
    png_names: Arc<BTreeSet<String>>,
) -> CoreResult<ModelPlan> {
    let all_textures = Arc::new(textures(Path::new("published"), &documents)?);
    let icon_paths = Arc::new(icon_texture_paths(inventory, &png_names)?);
    let mut candidates = BTreeSet::new();
    for resource in inventory.leaves() {
        if !matches!(
            extension(&resource.alias).to_ascii_lowercase().as_str(),
            "mds" | "mdt" | "mot" | "wgt" | "bbp" | "ico" | "stb" | "chr" | "chr_"
        ) {
            continue;
        }
        let mut ancestor = resource.alias.as_str();
        loop {
            if source.contains(ancestor) {
                candidates.insert(ancestor.to_owned());
                break;
            }
            let Some((parent, _)) = ancestor.rsplit_once('/') else {
                break;
            };
            ancestor = parent;
        }
    }
    for alias in inventory.names().filter(|alias| is_character(alias)) {
        let mut ancestor = alias;
        loop {
            if source.contains(ancestor) {
                candidates.insert(ancestor.to_owned());
                break;
            }
            let Some((parent, _)) = ancestor.rsplit_once('/') else {
                break;
            };
            ancestor = parent;
        }
    }
    let mut files: Vec<String> = source
        .names()
        .filter(|name| {
            let lower = name.to_ascii_lowercase();
            !lower.ends_with(".pss")
                && !matches!(lower.as_str(), "sound/sound.dat" | "sound/sound.hd3")
                && (candidates.contains(*name)
                    || source.resource(name).is_ok_and(|resource| {
                        resource
                            .bytes()
                            .windows(4)
                            .any(|signature| signature == b"MDS\0" || signature == b"MDT\0")
                    }))
        })
        .map(str::to_owned)
        .collect();
    files.sort_by_key(|alias| (!matches!(extension(alias), "mds" | "mdt"), alias.clone()));
    let analyses = Arc::new(Mutex::new(
        (0..files.len())
            .map(|_| None)
            .collect::<Vec<Option<Option<FileAnalysis>>>>(),
    ));
    let mut jobs = Vec::with_capacity(files.len());
    let mut deferred_work = 0u64;
    for (index, alias) in files.into_iter().enumerate() {
        let len = source.len(&alias)?;
        let work = 1 + len.min(8 << 20).div_ceil(256 << 10);
        deferred_work += work;
        let source = source.clone();
        let all_textures = all_textures.clone();
        let icon_paths = icon_paths.clone();
        let analyses = analyses.clone();
        jobs.push(Job {
            category: Category::Model,
            label: alias.clone(),
            work,
            estimated_bytes: (len as usize).saturating_mul(16).max(1 << 20),
            run: Box::new(move |_| {
                let mut analysis = analyze_file(
                    &source,
                    &alias,
                    &all_textures,
                    &icon_paths,
                    &Phase::noop(),
                    true,
                )?;
                if let Some(analysis) = analysis.as_mut() {
                    analysis.pack_artifacts()?;
                }
                analyses
                    .lock()
                    .map_err(|_| fail("model analysis lock poisoned"))?[index] = Some(analysis);
                Ok(())
            }),
        });
    }
    deferred_work = deferred_work.saturating_mul(MODEL_DEFERRED_WORK_MULTIPLIER);
    let mut consumed = BTreeSet::new();
    for resource in inventory.leaves() {
        let alias = resource.alias.as_str();
        if matches!(
            extension(alias).to_ascii_lowercase().as_str(),
            "mds" | "mdt" | "mot" | "wgt" | "bbp" | "ico" | "clo"
        ) {
            consumed.insert(alias.to_owned());
        }
    }
    for name in inventory.names() {
        if is_character(name) {
            consumed.insert(name.to_owned());
        }
    }
    let embedded_texture_sources = documents
        .iter()
        .filter(|document| character_parent(&document.source_alias).is_some())
        .map(|document| document.source_alias.clone())
        .collect();
    Ok(ModelPlan {
        consumed,
        embedded_texture_sources,
        deferred_work,
        jobs,
        analyses,
        documents,
        png_names,
    })
}

/// Publishes all model resources and removes their proprietary payloads from the compatibility tree.
pub fn convert_tree(
    raw: &Path,
    published: &Path,
    documents: &[super::textures::ConvertedTextureDocument],
    phase: &Phase,
) -> CoreResult<ConversionReport> {
    convert_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        documents,
        phase,
    )
}

pub fn convert_source(
    source: &super::archive::SourceTree,
    published: &Path,
    documents: &[super::textures::ConvertedTextureDocument],
    phase: &Phase,
) -> CoreResult<ConversionReport> {
    let mut report = ConversionReport::default();
    let mut files: Vec<&str> = source
        .names()
        .filter(|name| {
            let lower = name.to_ascii_lowercase();
            !lower.ends_with(".pss")
                && !matches!(lower.as_str(), "sound/sound.dat" | "sound/sound.hd3")
        })
        .collect();
    files.sort_by_key(|p| {
        (
            !matches!(
                Path::new(p).extension().and_then(|e| e.to_str()),
                Some("mds" | "mdt")
            ),
            *p,
        )
    });
    let work: Vec<u64> = files
        .iter()
        .map(|name| -> CoreResult<u64> {
            let length = source.len(name)?;
            if length > 128 << 20 {
                return Ok(1);
            }
            let base = 1 + length.min(8 << 20).div_ceil(256 << 10);
            let multiplier = match extension(name) {
                "mds" | "mdt" => 4,
                "chr" | "chr_" | "mot" | "wgt" | "bbp" | "ico" => 2,
                _ => 1,
            };
            Ok(base * multiplier)
        })
        .collect::<CoreResult<_>>()?;
    let scan_work: u64 = work.iter().sum();
    let total = scan_work + (scan_work / 20).max(1);
    phase.set_total(total);
    let all_textures = textures(published, documents)?;
    let inventory = super::inventory::Inventory::build(source)?;
    let mut png_names = BTreeSet::new();
    for document in documents {
        png_names.extend(super::textures::picture_assets(
            &document.document_path,
            &document.document,
        )?);
    }
    let icon_paths = icon_texture_paths(&inventory, &png_names)?;
    let mut used_textures = BTreeSet::new();
    let mut catalog_resources = BTreeMap::<String, Value>::new();
    let mut converted = BTreeSet::new();
    let analyze = |chunk: &[&str]| -> Vec<CoreResult<Option<FileAnalysis>>> {
        chunk
            .par_iter()
            .map(|p| analyze_file(source, p, &all_textures, &icon_paths, phase, false))
            .collect()
    };
    let mut chunks = files.chunks(64).zip(work.chunks(64));
    let mut next = chunks
        .next()
        .map(|(chunk, units)| (chunk, units, analyze(chunk)));
    while let Some((chunk, units, analyses)) = next {
        let mut jobs = Vec::new();
        for (p, analysis) in chunk.iter().zip(analyses) {
            match analysis {
                Ok(Some(analysis)) => jobs.extend(commit_file(
                    published,
                    analysis,
                    &mut converted,
                    &mut catalog_resources,
                    &mut used_textures,
                    &mut report,
                    false,
                )?),
                Ok(None) => {}
                Err(error) => {
                    if !converted.contains(*p) {
                        return Err(error);
                    }
                }
            }
        }
        // Analysis never reads committed state, so the next chunk is analysed while this one publishes.
        let (published_jobs, following) = rayon::join(
            || publish_jobs(published, jobs, documents, &mut report),
            || {
                chunks
                    .next()
                    .map(|(chunk, units)| (chunk, units, analyze(chunk)))
            },
        );
        published_jobs?;
        phase.inc(units.iter().sum());
        next = following;
    }
    publish_external(published, &mut report)?;
    phase.set_done(total);
    phase.log(format!(
        "models: {} plain, {} rebaked (max pose error {:.2e}), {} shear helpers, {} boundary keys, {} external motions",
        report.transforms.plain,
        report.transforms.baked,
        report.transforms.baked_error,
        report.transforms.helpers.len(),
        report.transforms.boundary_keys,
        report.externals.len()
    ));
    for helper in &report.transforms.helpers {
        phase.log(format!("models: shear helper {helper}"));
    }
    for unresolved in &report.unresolved {
        phase.log(format!("models: unresolved external motion {unresolved}"));
    }
    report.converted = converted;
    Ok(report)
}

/// Removes converted model sources from the data tree and scrubs model members from the rest.
pub fn strip_data(
    raw: &Path,
    published: &Path,
    report: &ConversionReport,
    phase: &Phase,
) -> CoreResult<()> {
    strip_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        report,
        phase,
    )
}

pub fn strip_source(
    source: &super::archive::SourceTree,
    published: &Path,
    report: &ConversionReport,
    phase: &Phase,
) -> CoreResult<()> {
    let converted = &report.converted;
    let mut files: Vec<&str> = source.names().collect();
    files.sort();
    phase.add_total(files.len() as u64);
    let results: Vec<CoreResult<()>> = files
        .par_iter()
        .map(|p| {
            let alias = *p;
            let dest = super::archive::compatibility_path(published, p);
            if converted.contains(alias) {
                if dest.is_file() {
                    fs::remove_file(dest)?
                }
            } else if dest.is_file() && fs::metadata(&dest)?.len() <= 128 << 20 {
                let mut b = fs::read(&dest)?;
                if scrub(&mut b, alias, converted, 0)? {
                    fs::write(dest, b)?
                }
            }
            phase.inc(1);
            Ok(())
        })
        .collect();
    results.into_iter().collect()
}

fn compact_pack(original: &[u8]) -> CoreResult<Option<Vec<u8>>> {
    let Some(members) = pack(original) else {
        return Ok(Some(original.to_vec()));
    };
    let mut retained = Vec::new();
    for member in members {
        if member.size == 0 || is_character(&member.name) {
            continue;
        }
        if let Some(payload) = compact_pack(bytes(original, member.start, member.size)?)? {
            retained.push((member.name, payload));
        }
    }
    if retained.is_empty() {
        return Ok(None);
    }
    let mut result = Vec::new();
    for (name, payload) in retained {
        let advance = 80usize
            .checked_add(payload.len().next_multiple_of(16))
            .ok_or_else(|| fail("PACK size overflow"))?;
        let start = result.len();
        result.resize(start + advance, 0);
        let name = name.as_bytes();
        if name.len() >= 64 {
            return Err(fail("PACK name exceeds header"));
        }
        result[start..start + name.len()].copy_from_slice(name);
        result[start + 64..start + 68].copy_from_slice(&80u32.to_le_bytes());
        result[start + 68..start + 72].copy_from_slice(
            &u32::try_from(payload.len())
                .map_err(|_| fail("PACK payload too large"))?
                .to_le_bytes(),
        );
        result[start + 72..start + 76].copy_from_slice(
            &u32::try_from(advance)
                .map_err(|_| fail("PACK advance too large"))?
                .to_le_bytes(),
        );
        result[start + 80..start + 80 + payload.len()].copy_from_slice(&payload);
    }
    result.resize(result.len() + 80, 0);
    Ok(Some(result))
}

/// Removes migrated archive members while preserving unrelated payloads in valid PACK records.
pub fn compact_remaining_packs(root: &Path) -> CoreResult<()> {
    if !root.is_dir() {
        return Ok(());
    }
    let mut files = Vec::new();
    collect(root, &mut files)?;
    for path in files {
        if path.strip_prefix(root).is_err() {
            return Err(fail("PACK target outside data root"));
        }
        if path.extension().is_some_and(|e| {
            e.to_string_lossy()
                .trim_end_matches('_')
                .eq_ignore_ascii_case("chr")
        }) {
            fs::remove_file(path)?;
            continue;
        }
        if fs::metadata(&path)?.len() > 128 << 20 {
            continue;
        }
        let original = fs::read(&path)?;
        if pack(&original).is_none() {
            continue;
        }
        match compact_pack(&original)? {
            None => fs::remove_file(path)?,
            Some(result) if result != original => fs::write(path, result)?,
            _ => {}
        }
    }
    Ok(())
}

fn decode_uri(uri: &str, prefix: &str) -> CoreResult<Vec<u8>> {
    let encoded = uri
        .strip_prefix(prefix)
        .ok_or_else(|| fail("glTF asset is not embedded"))?;
    if !encoded.len().is_multiple_of(4) {
        return Err(fail("invalid glTF base64 length"));
    }
    let mut out = Vec::with_capacity(encoded.len() / 4 * 3);
    for (index, chunk) in encoded.as_bytes().as_chunks::<4>().0.iter().enumerate() {
        let mut value = 0u32;
        let mut padding = 0;
        for (i, &c) in chunk.iter().enumerate() {
            let n = match c {
                b'A'..=b'Z' => c - b'A',
                b'a'..=b'z' => c - b'a' + 26,
                b'0'..=b'9' => c - b'0' + 52,
                b'+' => 62,
                b'/' => 63,
                b'=' if i >= 2 && index + 1 == encoded.len() / 4 => {
                    padding += 1;
                    0
                }
                _ => return Err(fail("invalid glTF base64")),
            };
            if padding > 0 && c != b'=' {
                return Err(fail("invalid glTF base64 padding"));
            }
            value = (value << 6) | n as u32;
        }
        out.push((value >> 16) as u8);
        if padding < 2 {
            out.push((value >> 8) as u8)
        }
        if padding < 1 {
            out.push(value as u8)
        }
    }
    Ok(out)
}
/// Resolves a relative glTF URI against its data document into a data path.
fn resolve_uri(document: &str, uri: &str) -> CoreResult<String> {
    let invalid = || fail(format!("invalid glTF image URI {uri:?} in {document}"));
    if uri.is_empty() || uri.starts_with('/') || uri.contains([':', '?', '#', '\\']) {
        return Err(invalid());
    }
    let mut parts: Vec<String> = document.split('/').map(str::to_owned).collect();
    parts.pop();
    for segment in uri.split('/') {
        let mut decoded = Vec::with_capacity(segment.len());
        let mut bytes = segment.bytes();
        while let Some(byte) = bytes.next() {
            if byte == b'%' {
                let hex = [
                    bytes.next().ok_or_else(invalid)?,
                    bytes.next().ok_or_else(invalid)?,
                ];
                let hex = std::str::from_utf8(&hex).map_err(|_| invalid())?;
                decoded.push(u8::from_str_radix(hex, 16).map_err(|_| invalid())?);
            } else {
                decoded.push(byte);
            }
        }
        let decoded = String::from_utf8(decoded).map_err(|_| invalid())?;
        if segment == ".." {
            parts.pop().ok_or_else(invalid)?;
        } else if matches!(decoded.as_str(), "" | "." | "..") || decoded.contains(['/', '\\', '\0'])
        {
            return Err(invalid());
        } else {
            parts.push(decoded);
        }
    }
    Ok(parts.join("/"))
}
/// Checks that every glTF image references an existing standalone PNG in data.
fn verify_images(
    port: &Path,
    document: &str,
    doc: &Value,
    metadata: &Value,
    files: &BTreeSet<String>,
    checked: &mut BTreeSet<String>,
) -> CoreResult<()> {
    let mut resolved = Vec::new();
    for image in doc["images"].as_array().into_iter().flatten() {
        let uri = image["uri"]
            .as_str()
            .ok_or_else(|| fail(format!("glTF image URI missing: {document}")))?;
        let path = resolve_uri(document, uri)?;
        if !files.contains(&path) {
            return Err(fail(format!("missing glTF image {path}: {document}")));
        }
        if !checked.contains(&path) {
            let mut signature = [0; 8];
            std::io::Read::read_exact(&mut fs::File::open(port.join(&path))?, &mut signature)?;
            if &signature != b"\x89PNG\r\n\x1a\n" {
                return Err(fail(format!("glTF image is not PNG: {path}")));
            }
            checked.insert(path.clone());
        }
        resolved.push(path);
    }
    for row in metadata["images"].as_array().into_iter().flatten() {
        let image = index(&row["image"], resolved.len(), "metadata image")?;
        if row["png"].as_str() != Some(resolved[image].as_str()) {
            return Err(fail(format!(
                "metadata image {image} does not match its glTF URI: {document}"
            )));
        }
    }
    Ok(())
}
fn index(value: &Value, limit: usize, what: &str) -> CoreResult<usize> {
    let i = value
        .as_u64()
        .ok_or_else(|| fail(format!("invalid glTF {what} index")))? as usize;
    if i >= limit {
        return Err(fail(format!("glTF {what} index outside array")));
    }
    Ok(i)
}
#[cfg(test)]
fn validate_gltf(doc: &Value, metadata: &Value) -> CoreResult<()> {
    let mut buffers = Vec::new();
    for b in doc["buffers"].as_array().into_iter().flatten() {
        buffers.push(decode_uri(
            b["uri"]
                .as_str()
                .ok_or_else(|| fail("glTF buffer URI missing"))?,
            "data:application/octet-stream;base64,",
        )?);
    }
    validate_gltf_buffers(doc, metadata, &buffers)
}
/// Validates a glTF document and its native metadata against already loaded buffer bytes.
fn validate_gltf_buffers(doc: &Value, metadata: &Value, buffers: &[Vec<u8>]) -> CoreResult<()> {
    fn null_standard(value: &Value) -> bool {
        match value {
            Value::Object(object) => object.iter().any(|(key, value)| {
                !matches!(key.as_str(), "extras" | "extensions") && null_standard(value)
            }),
            Value::Array(values) => values.iter().any(null_standard),
            Value::Null => true,
            _ => false,
        }
    }
    if null_standard(doc) {
        return Err(fail("glTF standard properties cannot be null"));
    }
    if doc["asset"]["version"] != "2.0" || metadata.get("schema_version").is_some_and(|v| v != 1) {
        return Err(fail("unsupported model schema"));
    }
    for key in [
        "accessors",
        "animations",
        "buffers",
        "bufferViews",
        "images",
        "materials",
        "meshes",
        "nodes",
        "samplers",
        "scenes",
        "skins",
        "textures",
    ] {
        if doc.get(key).is_some_and(|value| !value.is_array()) {
            return Err(fail(format!("glTF {key} must be an array")));
        }
    }
    let declared = doc["buffers"].as_array().map_or(&[][..], Vec::as_slice);
    if declared.len() != buffers.len() {
        return Err(fail("glTF buffer count mismatch"));
    }
    for (b, data) in declared.iter().zip(buffers) {
        if b["byteLength"].as_u64() != Some(data.len() as u64) {
            return Err(fail("glTF buffer length mismatch"));
        }
    }
    let empty = Vec::new();
    let views = doc["bufferViews"].as_array().unwrap_or(&empty);
    let accessors = doc["accessors"].as_array().unwrap_or(&empty);
    let nodes = doc["nodes"].as_array().unwrap_or(&empty);
    for v in views {
        let b = index(&v["buffer"], buffers.len(), "buffer")?;
        let start = v["byteOffset"].as_u64().unwrap_or(0) as usize;
        let length = v["byteLength"]
            .as_u64()
            .ok_or_else(|| fail("glTF view length missing"))? as usize;
        if length == 0
            || start
                .checked_add(length)
                .is_none_or(|end| end > buffers[b].len())
        {
            return Err(fail("glTF view outside buffer"));
        }
    }
    for a in accessors {
        if a["bufferView"].is_null() {
            let sparse = &a["sparse"];
            let count = sparse["count"]
                .as_u64()
                .ok_or_else(|| fail("sparse accessor count missing"))?
                as usize;
            if count == 0 || Some(count as u64) > a["count"].as_u64() {
                return Err(fail("invalid sparse accessor count"));
            }
            let iv = &views[index(
                &sparse["indices"]["bufferView"],
                views.len(),
                "sparse indices",
            )?];
            let vv = &views[index(
                &sparse["values"]["bufferView"],
                views.len(),
                "sparse values",
            )?];
            if sparse["indices"]["componentType"] != 5125
                || iv["byteLength"].as_u64() != Some((count * 4) as u64)
                || vv["byteLength"].as_u64() != Some((count * 12) as u64)
            {
                return Err(fail("invalid sparse accessor views"));
            }
            let buffer = &buffers[index(&iv["buffer"], buffers.len(), "sparse buffer")?];
            let start = iv["byteOffset"].as_u64().unwrap_or(0) as usize;
            let mut previous = None;
            for i in 0..count {
                let vertex = u32le(buffer, start + i * 4)?;
                if vertex as u64 >= a["count"].as_u64().unwrap()
                    || previous.is_some_and(|p| p >= vertex)
                {
                    return Err(fail("sparse indices are unordered or outside accessor"));
                }
                previous = Some(vertex);
            }
            continue;
        }
        let v = &views[index(&a["bufferView"], views.len(), "view")?];
        let width = match a["type"].as_str() {
            Some("SCALAR") => 1,
            Some("VEC2") => 2,
            Some("VEC3") => 3,
            Some("VEC4") => 4,
            Some("MAT4") => 16,
            _ => return Err(fail("unsupported glTF accessor type")),
        };
        let component = match a["componentType"].as_u64() {
            Some(5123) => 2,
            Some(5126) => 4,
            _ => return Err(fail("unsupported glTF component type")),
        };
        let count = a["count"]
            .as_u64()
            .ok_or_else(|| fail("accessor count missing"))? as usize;
        let offset = a["byteOffset"].as_u64().unwrap_or(0) as usize;
        let stride = v["byteStride"]
            .as_u64()
            .unwrap_or((width * component) as u64) as usize;
        if count == 0
            || !offset.is_multiple_of(component)
            || offset
                .checked_add(
                    (count - 1)
                        .checked_mul(stride)
                        .and_then(|n| n.checked_add(width * component))
                        .ok_or_else(|| fail("accessor overflow"))?,
                )
                .is_none_or(|n| n > v["byteLength"].as_u64().unwrap() as usize)
        {
            return Err(fail("glTF accessor outside view"));
        }
    }
    for image in doc["images"].as_array().unwrap_or(&empty) {
        let uri = image["uri"]
            .as_str()
            .ok_or_else(|| fail("glTF image URI missing"))?;
        if uri.starts_with("data:") {
            return Err(fail("glTF image is embedded instead of a standalone PNG"));
        }
    }
    let meshes = doc["meshes"].as_array().unwrap_or(&empty);
    let skins = doc["skins"].as_array().unwrap_or(&empty);
    for node in nodes {
        if !node["mesh"].is_null() {
            index(&node["mesh"], meshes.len(), "mesh")?;
        }
        if !node["skin"].is_null() {
            index(&node["skin"], skins.len(), "skin")?;
        }
        for child in node["children"].as_array().unwrap_or(&empty) {
            index(child, nodes.len(), "child")?;
        }
    }
    for skin in skins {
        let joints = skin["joints"]
            .as_array()
            .ok_or_else(|| fail("skin joints missing"))?;
        if joints.is_empty() {
            return Err(fail("invalid model skin joint count"));
        }
        for joint in joints {
            index(joint, nodes.len(), "joint")?;
        }
        let a = &accessors[index(&skin["inverseBindMatrices"], accessors.len(), "bind pose")?];
        if a["type"] != "MAT4" || a["count"].as_u64() != Some(joints.len() as u64) {
            return Err(fail("skin bind pose count mismatch"));
        }
    }
    for mesh in meshes {
        for primitive in mesh["primitives"]
            .as_array()
            .ok_or_else(|| fail("mesh primitives missing"))?
        {
            let attributes = primitive["attributes"]
                .as_object()
                .ok_or_else(|| fail("primitive attributes missing"))?;
            let position = &accessors[index(
                &primitive["attributes"]["POSITION"],
                accessors.len(),
                "position",
            )?];
            for a in attributes.values() {
                let accessor = &accessors[index(a, accessors.len(), "attribute")?];
                if accessor["count"] != position["count"] {
                    return Err(fail("primitive attribute count mismatch"));
                }
            }
            if !primitive["material"].is_null() {
                index(
                    &primitive["material"],
                    doc["materials"].as_array().unwrap_or(&empty).len(),
                    "material",
                )?;
            }
        }
    }
    for animation in doc["animations"].as_array().unwrap_or(&empty) {
        let samplers = animation["samplers"]
            .as_array()
            .ok_or_else(|| fail("animation samplers missing"))?;
        for sampler in samplers {
            let input = &accessors[index(&sampler["input"], accessors.len(), "animation input")?];
            let output =
                &accessors[index(&sampler["output"], accessors.len(), "animation output")?];
            if input["type"] != "SCALAR"
                || output["count"].as_u64().unwrap_or(0) < input["count"].as_u64().unwrap_or(0)
            {
                return Err(fail("animation sampler count mismatch"));
            }
        }
        for channel in animation["channels"]
            .as_array()
            .ok_or_else(|| fail("animation channels missing"))?
        {
            let sampler =
                &samplers[index(&channel["sampler"], samplers.len(), "animation sampler")?];
            let node = index(&channel["target"]["node"], nodes.len(), "animation target")?;
            let input = &accessors[index(&sampler["input"], accessors.len(), "animation input")?];
            let output =
                &accessors[index(&sampler["output"], accessors.len(), "animation output")?];
            let multiplier = if channel["target"]["path"] == "weights" {
                let mesh = index(&nodes[node]["mesh"], meshes.len(), "morph mesh")?;
                meshes[mesh]["primitives"][0]["targets"]
                    .as_array()
                    .ok_or_else(|| fail("morph targets missing"))?
                    .len() as u64
            } else {
                1
            };
            if input["count"].as_u64().unwrap().checked_mul(multiplier) != output["count"].as_u64()
            {
                return Err(fail("animation output sample count mismatch"));
            }
        }
    }
    if let Some(frames) = metadata["frames"].as_array() {
        if metadata["node_offset"].as_u64().unwrap_or(0) as usize + frames.len() > nodes.len() {
            return Err(fail("model frame count mismatch"));
        }
        for (id, frame) in frames.iter().enumerate() {
            if let Some(helper) = frame.get("rotation_node") {
                let helper = index(helper, nodes.len(), "rotation helper")?;
                let original = metadata["node_offset"].as_u64().unwrap_or(0) as usize + id;
                if helper == original
                    || !nodes[original]["children"]
                        .as_array()
                        .is_some_and(|children| {
                            children
                                .iter()
                                .any(|child| child.as_u64() == Some(helper as u64))
                        })
                {
                    return Err(fail("rotation helper is not a child of its source frame"));
                }
            }
            if frame
                .get("id")
                .is_some_and(|value| value.as_u64() != Some(id as u64))
            {
                return Err(fail("invalid retail frame metadata"));
            }
        }
    }
    Ok(())
}

/// Validates one model's clips of an animation resource against its actions and skeleton.
fn validate_animation_variant(
    doc: &Value,
    metadata: &Value,
    resource: &Value,
    variant: &Value,
) -> CoreResult<()> {
    let accessors = doc["accessors"].as_array().map_or(&[][..], Vec::as_slice);
    let model = index(
        &variant["model"],
        metadata["models"].as_array().map_or(0, Vec::len),
        "animation model",
    )?;
    let model = &metadata["models"][model];
    let offset = model["node_offset"].as_u64().unwrap_or(0) as usize;
    if variant["node_offset"].as_u64() != Some(offset as u64) {
        return Err(fail("animation/model skeleton offset mismatch"));
    }
    let frames = model["frames"]
        .as_array()
        .ok_or_else(|| fail("animation model frames missing"))?;
    let mut owned: BTreeSet<usize> = (offset..offset + frames.len()).collect();
    for frame in frames {
        if let Some(helper) = frame["rotation_node"].as_u64() {
            owned.insert(helper as usize);
        }
    }
    let actions = resource["actions"].as_array().unwrap();
    let clips = variant["animations"]
        .as_array()
        .ok_or_else(|| fail("animation variant clips missing"))?;
    if clips.len() != actions.len() {
        return Err(fail(
            "animation variant clip count differs from its actions",
        ));
    }
    for (action, clip) in actions.iter().zip(clips) {
        if clip.is_null() {
            continue;
        }
        let clip = index(
            clip,
            doc["animations"].as_array().map_or(0, Vec::len),
            "animation clip",
        )?;
        let animation = &doc["animations"][clip];
        let name = animation["name"].as_str().unwrap_or("");
        let base = name.split('@').next().unwrap_or("");
        let expected = action["name"].as_str().unwrap_or("");
        let base = if resource.get("external").is_some() {
            base.rsplit_once('/').map_or("", |p| p.1)
        } else {
            base
        };
        if base != expected {
            return Err(fail(format!(
                "animation {name} does not name its action {expected}"
            )));
        }
        for channel in animation["channels"].as_array().unwrap() {
            let node = channel["target"]["node"].as_u64().unwrap() as usize;
            if !owned.contains(&node) {
                return Err(fail(format!(
                    "animation {name} targets a node outside its model"
                )));
            }
        }
        for sampler in animation["samplers"].as_array().unwrap() {
            let input = &accessors[sampler["input"].as_u64().unwrap() as usize];
            if input["min"][0].as_f64().is_none_or(|min| min < 0.) {
                return Err(fail(format!("animation {name} starts before its action")));
            }
        }
    }
    Ok(())
}

/// Validates an animation resource's named actions.
fn validate_actions(resource: &Value) -> CoreResult<()> {
    let actions = resource["actions"]
        .as_array()
        .ok_or_else(|| fail("animation actions missing"))?;
    let mut names = BTreeSet::new();
    for action in actions {
        let name = action["name"].as_str().unwrap_or("");
        if name.is_empty() || !names.insert(name) {
            return Err(fail(format!(
                "animation action name empty or repeated: {name}"
            )));
        }
        if action["duration"].as_u64().is_none() {
            return Err(fail(format!("animation action {name} lacks a duration")));
        }
    }
    Ok(())
}

/// Checks that every English motion name stands for the original name it translates.
fn validate_game_names(metadata: &Value) -> CoreResult<BTreeSet<String>> {
    let mut names = BTreeSet::new();
    for (name, game) in metadata["game_names"].as_object().into_iter().flatten() {
        let text = game["value"]
            .as_str()
            .filter(|_| game["type"] == "string")
            .ok_or_else(|| fail(format!("game name of {name} is not a string")))?;
        let base = name
            .rsplit_once("_v")
            .filter(|(_, n)| n.parse::<u32>().is_ok())
            .map_or(name.as_str(), |p| p.0);
        if super::actions::english(text)? != base {
            return Err(fail(format!("{name} does not translate {text}")));
        }
        names.insert(name.clone());
    }
    Ok(names)
}

/// Checks that profiles name actions of their motions and known original names.
fn validate_profile_names(metadata: &Value, names: &BTreeSet<String>) -> CoreResult<()> {
    let animations = metadata["resources"]["animations"].as_array().unwrap();
    for (profile, document) in metadata["profiles"].as_object().into_iter().flatten() {
        let mut actions = BTreeSet::new();
        for command in document["commands"].as_array().into_iter().flatten() {
            let args = command["args"].as_array().map_or(&[][..], Vec::as_slice);
            match command["command"].as_str().unwrap_or("") {
                "MOTION" => {
                    actions = args
                        .get(1)
                        .and_then(|a| a["index"].as_u64())
                        .and_then(|i| animations.get(i as usize))
                        .and_then(|a| a["actions"].as_array())
                        .into_iter()
                        .flatten()
                        .filter_map(|a| a["name"].as_str().map(str::to_owned))
                        .collect();
                }
                "KEY" => {
                    let name = args.first().and_then(|a| a["value"].as_str()).unwrap_or("");
                    if args.len() != 2 || !actions.contains(name) || !names.contains(name) {
                        return Err(fail(format!("{profile}: KEY {name} names no action")));
                    }
                }
                "SE" | "SELP" | "SEQ" | "SEQ_START" => {
                    let name = args.first().and_then(|a| a["value"].as_str()).unwrap_or("");
                    if !names.contains(name) {
                        return Err(fail(format!(
                            "{profile}: {} names unknown motion {name}",
                            command["command"]
                        )));
                    }
                }
                _ => {}
            }
        }
    }
    Ok(())
}

/// Verifies direct glTF assets, embedded semantic resources and remaining retail payloads.
pub fn verify(root: &Path) -> CoreResult<()> {
    let port = root.join("data");
    let mut files = Vec::new();
    if port.is_dir() {
        collect(&port, &mut files)?;
    }
    let names: BTreeSet<String> = files
        .iter()
        .map(|path| {
            path.strip_prefix(&port)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/")
        })
        .collect();
    verify_files(root, &names, &Phase::noop())?;
    for name in &names {
        let file = port.join(name);
        if fs::metadata(&file)?.len() <= 128 << 20 {
            audit_payload(name, &fs::read(file)?)?;
        }
    }
    Ok(())
}

/// Counts glTF documents that require semantic verification.
pub fn verify_work(names: &BTreeSet<String>) -> u64 {
    names.iter().filter(|name| name.ends_with(".gltf")).count() as u64
}

/// Verifies models against a previously collected data-tree inventory.
pub fn verify_files(root: &Path, names: &BTreeSet<String>, phase: &Phase) -> CoreResult<()> {
    verify_files_with_script_policy(root, names, phase, true)
}

/// Verifies models while allowing omitted compiled script paths only under Luau-only publication.
pub fn verify_files_with_script_policy(
    root: &Path,
    names: &BTreeSet<String>,
    phase: &Phase,
    publish_scripts: bool,
) -> CoreResult<()> {
    let port = root.join("data");
    let documents: Vec<PathBuf> = names
        .iter()
        .filter(|name| name.ends_with(".gltf"))
        .map(|name| port.join(name))
        .collect();
    let checked: Vec<CoreResult<BTreeSet<String>>> = documents
        .par_iter()
        .map(|path| -> CoreResult<BTreeSet<String>> {
            let result = (|| -> CoreResult<BTreeSet<String>> {
                let mut checked_pngs = BTreeSet::new();
                let mut buffer_files = BTreeSet::new();
                let doc: Value = serde_json::from_slice(&fs::read(path)?)?;
                let metadata: Value = if doc["asset"]["generator"] == "Kazarov Memory Card Icon" {
                    json!({"schema_version":1})
                } else {
                    serde_json::from_slice(&fs::read(path.with_extension("game.json"))?)?
                };
                if doc.get("extras").is_some() {
                    return Err(fail("custom model extras must be in the semantic sidecar"));
                }
                if metadata["schema_version"] != 1 {
                    return Err(fail(format!(
                        "unsupported native glTF metadata: {}",
                        path.display()
                    )));
                }
                let document = path
                    .strip_prefix(&port)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                for buffer in doc["buffers"].as_array().into_iter().flatten() {
                    let uri = buffer["uri"].as_str().unwrap_or("");
                    if uri.starts_with("data:") {
                        continue;
                    }
                    let target = resolve_uri(&document, uri)?;
                    if target != buffer_path(&document) || !names.contains(&target) {
                        return Err(fail(format!("invalid glTF buffer {uri}: {document}")));
                    }
                    buffer_files.insert(target);
                }
                let buffers = load_buffers(&port, &document, &doc)?;
                validate_gltf_buffers(&doc, &metadata, &buffers)?;
                let animations = doc["animations"].as_array().map_or(&[][..], Vec::as_slice);
                let has_model = doc["meshes"].as_array().is_some_and(|m| !m.is_empty())
                    || doc["skins"].as_array().is_some_and(|s| !s.is_empty())
                    || metadata["models"].as_array().is_some_and(|models| {
                        models
                            .iter()
                            .any(|m| m["frames"].as_array().is_some_and(|f| !f.is_empty()))
                    })
                    || metadata["frames"].as_array().is_some_and(|f| !f.is_empty());
                if !animations.is_empty() && !has_model {
                    return Err(fail(format!("animation-only glTF: {}", path.display())));
                }
                let mut clip_names = BTreeSet::new();
                for animation in animations {
                    let name = animation["name"].as_str().unwrap_or("");
                    if name.is_empty() || !clip_names.insert(name) {
                        return Err(fail(format!(
                            "animation name empty or repeated in {}: {name}",
                            path.display()
                        )));
                    }
                }
                verify_images(&port, &document, &doc, &metadata, names, &mut checked_pngs)?;
                if metadata["kind"] == "character" {
                    for model in metadata["models"]
                        .as_array()
                        .ok_or_else(|| fail("character models missing"))?
                    {
                        validate_gltf_buffers(&doc, model, &buffers)?;
                        index(
                            &model["scene"],
                            doc["scenes"].as_array().map_or(0, Vec::len),
                            "character model scene",
                        )?;
                    }
                    let resources = &metadata["resources"];
                    for key in ["scripts", "children"] {
                        for resource in resources[key]
                            .as_array()
                            .ok_or_else(|| fail("character resource array missing"))?
                        {
                            let resource_path = resource["path"]
                                .as_str()
                                .ok_or_else(|| fail("character resource path missing"))?;
                            if !names.contains(resource_path)
                                && !may_omit_character_resource(key, resource_path, publish_scripts)
                            {
                                return Err(fail(format!(
                                    "missing character resource: {resource_path}"
                                )));
                            }
                        }
                    }
                    for animation in resources["animations"]
                        .as_array()
                        .ok_or_else(|| fail("character animations missing"))?
                    {
                        validate_actions(animation)?;
                        for variant in animation["variants"].as_array().into_iter().flatten() {
                            validate_animation_variant(&doc, &metadata, animation, variant)?;
                        }
                        if animation["variants"].is_null() {
                            let external = animation["external"].as_str().ok_or_else(|| {
                                fail("animation without clips or external identity")
                            })?;
                            if !external.starts_with(&format!("{document}#")) {
                                return Err(fail(format!(
                                    "external motion {external} names another bundle"
                                )));
                            }
                            for target in animation["targets"]
                                .as_array()
                                .ok_or_else(|| fail("external motion targets missing"))?
                            {
                                if !target.as_str().is_some_and(|t| names.contains(t)) {
                                    return Err(fail(format!(
                                        "external motion target missing: {target}"
                                    )));
                                }
                            }
                        }
                    }
                    let motion_names = validate_game_names(&metadata)?;
                    validate_profile_names(&metadata, &motion_names)?;
                    for profile in metadata["profiles"]
                        .as_object()
                        .ok_or_else(|| fail("character profiles missing"))?
                        .values()
                    {
                        for command in profile["commands"]
                            .as_array()
                            .ok_or_else(|| fail("character commands missing"))?
                        {
                            for arg in command["args"]
                                .as_array()
                                .ok_or_else(|| fail("character command arguments missing"))?
                            {
                                if arg["type"] != "resource" || arg["index"].is_null() {
                                    continue;
                                }
                                let key = match arg["kind"].as_str() {
                                    Some("model") => "models",
                                    Some("animation") => "animations",
                                    Some("bind_pose") => "bind_poses",
                                    Some("weights") => "weights",
                                    Some("texture_set") => "texture_sets",
                                    Some("cloth") => "cloths",
                                    Some("character") => "children",
                                    Some("script") => "scripts",
                                    _ => return Err(fail("unknown character resource kind")),
                                };
                                let count = if key == "models" {
                                    metadata[key].as_array()
                                } else {
                                    resources[key].as_array()
                                }
                                .map_or(0, Vec::len);
                                index(&arg["index"], count, "character command resource")?;
                            }
                        }
                    }
                    for set in resources["texture_sets"]
                        .as_array()
                        .ok_or_else(|| fail("character texture sets missing"))?
                    {
                        if set["document"]["schema_version"] != 3 {
                            return Err(fail("invalid embedded texture schema"));
                        }
                        for entry in set["document"]["entries"]
                            .as_array()
                            .ok_or_else(|| fail("embedded texture entries missing"))?
                        {
                            let Some(pictures) = entry["pictures"].as_array() else {
                                if entry["script"].is_array() {
                                    continue;
                                } else {
                                    return Err(fail("embedded texture pictures missing"));
                                }
                            };
                            for picture in pictures {
                                let mut levels = Vec::new();
                                for key in ["levels", "variants"] {
                                    if picture.get(key).is_some_and(|value| !value.is_array()) {
                                        return Err(fail(format!(
                                            "embedded texture {key} is not an array"
                                        )));
                                    }
                                }
                                if let Some(values) = picture["levels"].as_array() {
                                    levels.extend(values);
                                }
                                if let Some(variants) = picture["variants"].as_array() {
                                    for variant in variants {
                                        levels.extend(variant["levels"].as_array().ok_or_else(
                                            || fail("embedded texture variant levels missing"),
                                        )?);
                                    }
                                }
                                for level in levels {
                                    let id = level
                                        .as_str()
                                        .and_then(|s| s.strip_prefix("#/images/"))
                                        .and_then(|s| s.parse::<usize>().ok())
                                        .ok_or_else(|| {
                                            fail("invalid embedded texture image URI")
                                        })?;
                                    if id >= doc["images"].as_array().map_or(0, Vec::len) {
                                        return Err(fail("embedded texture image out of range"));
                                    }
                                }
                            }
                        }
                    }
                }
                Ok(buffer_files)
            })();
            phase.inc(1);
            result
        })
        .collect();
    let mut buffer_files = BTreeSet::new();
    for result in checked {
        buffer_files.extend(result?);
    }
    if let Some(orphan) = names
        .iter()
        .find(|name| name.ends_with(".gltf.bin") && !buffer_files.contains(*name))
    {
        return Err(fail(format!("glTF buffer without its document: {orphan}")));
    }
    Ok(())
}

/// Number of model documents checked after the archive stream is fully sealed.
pub fn verify_archive_work(names: &[String]) -> u64 {
    names.iter().filter(|name| name.ends_with(".gltf")).count() as u64
}

/// Validates model structure and cross-asset references directly from the final ZIP.
pub fn verify_archive(reader: &dyn super::publish::ArchiveReader, phase: &Phase) -> CoreResult<()> {
    verify_archive_with_script_policy(reader, phase, true)
}

pub fn verify_archive_with_script_policy(
    reader: &dyn super::publish::ArchiveReader,
    phase: &Phase,
    publish_scripts: bool,
) -> CoreResult<()> {
    let names: BTreeSet<String> = reader.names().into_iter().collect();
    let documents: Vec<String> = names
        .iter()
        .filter(|name| name.ends_with(".gltf"))
        .cloned()
        .collect();
    let checked: Vec<CoreResult<BTreeSet<String>>> = documents
        .par_iter()
        .map(|document| {
            let result = (|| -> CoreResult<BTreeSet<String>> {
                let doc: Value = serde_json::from_slice(&reader.read(document)?)?;
                let icon = doc["asset"]["generator"] == "Kazarov Memory Card Icon";
                let metadata: Value = if icon {
                    json!({"schema_version":1})
                } else {
                    let sidecar = Path::new(document)
                        .with_extension("game.json")
                        .to_string_lossy()
                        .replace('\\', "/");
                    serde_json::from_slice(&reader.read(&sidecar)?)?
                };
                if doc.get("extras").is_some() || metadata["schema_version"] != 1 {
                    return Err(fail(format!("invalid native glTF metadata: {document}")));
                }
                let mut buffers = Vec::new();
                let mut buffer_files = BTreeSet::new();
                for buffer in doc["buffers"].as_array().into_iter().flatten() {
                    let uri = buffer["uri"]
                        .as_str()
                        .ok_or_else(|| fail("glTF buffer URI missing"))?;
                    if uri.starts_with("data:") {
                        buffers.push(decode_uri(uri, "data:application/octet-stream;base64,")?);
                    } else {
                        let target = resolve_uri(document, uri)?;
                        if target != buffer_path(document) || !reader.contains(&target) {
                            return Err(fail(format!("invalid glTF buffer {uri}: {document}")));
                        }
                        buffers.push(reader.read(&target)?);
                        buffer_files.insert(target);
                    }
                }
                validate_gltf_buffers(&doc, &metadata, &buffers)?;
                let mut animation_names = BTreeSet::new();
                for animation in doc["animations"].as_array().into_iter().flatten() {
                    let name = animation["name"].as_str().unwrap_or("");
                    if name.is_empty() || !animation_names.insert(name) {
                        return Err(fail(format!(
                            "animation name empty or repeated in {document}: {name}"
                        )));
                    }
                }
                let mut image_paths = Vec::new();
                for image in doc["images"].as_array().into_iter().flatten() {
                    let uri = image["uri"]
                        .as_str()
                        .ok_or_else(|| fail("glTF image URI missing"))?;
                    let target = resolve_uri(document, uri)?;
                    if !reader.contains(&target) {
                        return Err(fail(format!("invalid model PNG {target}: {document}")));
                    }
                    image_paths.push(target);
                }
                for image in metadata["images"].as_array().into_iter().flatten() {
                    let index = index(&image["image"], image_paths.len(), "metadata image")?;
                    if image["png"].as_str() != Some(image_paths[index].as_str()) {
                        return Err(fail(format!(
                            "metadata image differs from glTF URI: {document}"
                        )));
                    }
                }
                if metadata["kind"] == "character" {
                    for model in metadata["models"]
                        .as_array()
                        .ok_or_else(|| fail("character models missing"))?
                    {
                        validate_gltf_buffers(&doc, model, &buffers)?;
                        index(
                            &model["scene"],
                            doc["scenes"].as_array().map_or(0, Vec::len),
                            "character model scene",
                        )?;
                    }
                    let resources = &metadata["resources"];
                    for key in ["scripts", "children"] {
                        for resource in resources[key]
                            .as_array()
                            .ok_or_else(|| fail("character resource array missing"))?
                        {
                            let path = resource["path"]
                                .as_str()
                                .ok_or_else(|| fail("character resource path missing"))?;
                            if !reader.contains(path)
                                && !may_omit_character_resource(key, path, publish_scripts)
                            {
                                return Err(fail(format!("missing character resource: {path}")));
                            }
                        }
                    }
                    for animation in resources["animations"]
                        .as_array()
                        .ok_or_else(|| fail("character animations missing"))?
                    {
                        validate_actions(animation)?;
                        for variant in animation["variants"].as_array().into_iter().flatten() {
                            validate_animation_variant(&doc, &metadata, animation, variant)?;
                        }
                    }
                    if metadata.get("profiles").is_some() || metadata.get("game_names").is_some() {
                        return Err(fail(format!(
                            "gameplay profile leaked into model archive: {document}"
                        )));
                    }
                }
                Ok(buffer_files)
            })();
            phase.inc(1);
            result
        })
        .collect();
    let mut used_buffers = BTreeSet::new();
    for result in checked {
        used_buffers.extend(result?);
    }
    if let Some(orphan) = names
        .iter()
        .find(|name| name.ends_with(".gltf.bin") && !used_buffers.contains(*name))
    {
        return Err(fail(format!("glTF buffer without document: {orphan}")));
    }
    Ok(())
}

/// Rejects model payloads that must have been converted before publication.
pub fn audit_payload(alias: &str, bytes: &[u8]) -> CoreResult<()> {
    if bytes.len() > 128 << 20 {
        return Ok(());
    }
    let mut resources = Vec::new();
    discover(bytes, alias, &mut resources, 0)?;
    if let Some(resource) = resources.iter().find(|resource| {
        resource.data.starts_with(b"MDS\0")
            || resource.data.starts_with(b"MDT\0")
            || matches!(
                extension(&resource.alias),
                "chr" | "chr_" | "mds" | "mdt" | "mot" | "wgt" | "bbp" | "ico"
            )
    }) {
        return Err(fail(format!(
            "proprietary model payload remains: {}",
            resource.alias
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn luau_only_allows_missing_script_paths_but_not_other_character_resources() {
        assert!(is_compiled_script_path("event/map/test.stb"));
        assert!(is_compiled_script_path("mainchr/c02.stb_"));
        assert!(!is_compiled_script_path("character/model.gltf"));
        assert!(!is_compiled_script_path("../outside.stb"));
        assert!(!is_compiled_script_path("event\\map\\test.stb"));
        assert!(may_omit_character_resource(
            "scripts",
            "event/map/test.stb",
            false
        ));
        assert!(may_omit_character_resource(
            "scripts",
            "mainchr/c02.stb_",
            false
        ));
        assert!(!may_omit_character_resource(
            "scripts",
            "character/model.gltf",
            false
        ));
        assert!(!may_omit_character_resource(
            "children",
            "event/map/test.stb",
            false
        ));
        assert!(!may_omit_character_resource(
            "scripts",
            "event/map/test.stb",
            true
        ));
        for index in 0..33 {
            let path = format!("event/e{index:03}/e{index:03}.stb");
            assert!(may_omit_character_resource("scripts", &path, false));
            assert!(!may_omit_character_resource("children", &path, false));
        }
    }

    use super::*;

    #[test]
    fn linked_copies_use_each_motion_index_when_placement_details_match() {
        let external = |key: &str| ExternalMotion {
            key: key.into(),
            sidecar: None,
            resource: 0,
            channels: Vec::new(),
            actions: Vec::new(),
        };
        let motions = vec![
            external("event/a/foo.gltf#0"),
            external("event/b/foo.gltf#0"),
            external("event/c/bar.gltf#0"),
        ];
        let placements = vec![
            json!({"method":"linked"}),
            json!({"method":"linked"}),
            json!({"method":"heuristic"}),
        ];
        let selected = vec![
            BTreeSet::from([1]),
            BTreeSet::from([2]),
            BTreeSet::from([3]),
        ];
        let (copies, names) =
            linked_copy_candidates(&motions, &placements, &selected, "event/orphan/foo.gltf");
        assert_eq!(copies, BTreeSet::from([1, 2]));
        assert_eq!(names, vec![motions[0].key.clone(), motions[1].key.clone()]);
    }

    #[test]
    fn external_motion_cost_moves_heavy_final_jobs_forward_without_changing_work_total() {
        let motion = ExternalMotion {
            key: "motion#0".into(),
            sidecar: None,
            resource: 0,
            channels: vec![json!({"type":1,"keys":[{}, {}, {}]})],
            actions: vec![("clip".into(), 0, 90)],
        };
        let external = external_motion_work(&motion);
        assert!(external > 90);
        let weights = final_model_weights(
            &[5_000_000_000, 1_000_000_000, 4_000_000_000],
            &[0, external, 0],
        );
        assert!(weights[1] > weights[0]);
        let work = apportion_model_work(&weights, 61);
        assert_eq!(work.iter().sum::<u64>(), 61);
        assert!(work.iter().all(|&item| item > 0));
        assert!(work[1] > work[0] && work[1] > work[2]);
        assert_eq!(apportion_model_work(&[0, 0, 0], 5).iter().sum::<u64>(), 5);
    }

    #[test]
    fn gameplay_audit_preserves_native_float_bits_and_rejects_edits() {
        let native = [0x3dcccccd_u32, 0x3f800001, 0x40490fdb].map(f32::from_bits);
        let expected = json!({"schema_version":1,"kind":"character_gameplay",
            "profiles":{"idle":{"commands":[{"command":"KEY","args":native}]}},
            "game_names":{}});
        let exported = serde_json::to_vec_pretty(&expected).unwrap();
        let actual: Value = serde_json::from_slice(&exported).unwrap();
        let normalized: Value =
            serde_json::from_slice(&serde_json::to_vec(&expected).unwrap()).unwrap();
        assert_eq!(actual, normalized);
        let args = actual["profiles"]["idle"]["commands"][0]["args"]
            .as_array()
            .unwrap();
        for (number, original) in args.iter().zip(native) {
            assert_eq!(
                (number.as_f64().unwrap() as f32).to_bits(),
                original.to_bits()
            );
        }
        let mut edited = actual.clone();
        edited["profiles"]["idle"]["commands"][0]["args"][1] = json!(2.0);
        assert_ne!(edited, normalized);
    }

    fn convert_and_strip(
        raw: &Path,
        published: &Path,
        documents: &[super::super::textures::ConvertedTextureDocument],
    ) -> CoreResult<ConversionReport> {
        let report = convert_tree(raw, published, documents, &Phase::noop())?;
        strip_data(raw, published, &report, &Phase::noop())?;
        Ok(report)
    }
    fn put(b: &mut [u8], at: usize, v: u32) {
        b[at..at + 4].copy_from_slice(&v.to_le_bytes())
    }
    #[test]
    fn malformed_scene_is_rejected() {
        let mut b = vec![0; 128];
        b[..4].copy_from_slice(b"MDS\0");
        put(&mut b, 8, 1);
        put(&mut b, 12, 16);
        assert!(scene_extent(&b).is_err());
    }
    #[test]
    fn flattened_icon_names_reserve_distinct_images_across_packs_and_textures() {
        let temporary = tempfile::tempdir().unwrap();
        for folder in ["menu/first.pac", "menu/second.pac"] {
            fs::create_dir_all(temporary.path().join(folder)).unwrap();
            fs::write(temporary.path().join(folder).join("icon.ico"), b"icon").unwrap();
        }
        let source = super::super::archive::SourceTree::directory(temporary.path()).unwrap();
        let inventory = super::super::inventory::Inventory::build(&source).unwrap();
        let existing = BTreeSet::from(["textures/menu/icon.png".to_owned()]);
        let paths = icon_texture_paths(&inventory, &existing).unwrap();
        assert_eq!(paths.len(), 2);
        let unique: BTreeSet<_> = paths.values().collect();
        assert_eq!(unique.len(), 2);
        for path in paths.values() {
            assert!(path.starts_with("textures/menu/icon-"));
            assert!(!existing.contains(path));
            assert!(path.len() <= 160);
        }
    }
    #[test]
    fn memory_card_icon_becomes_standard_gltf_without_unnecessary_sidecar() {
        let mut raw = vec![0; 20 + 3 * 24 + 20 + 128 * 128 * 2];
        put(&mut raw, 0, 0x10000);
        put(&mut raw, 4, 1);
        put(&mut raw, 16, 3);
        for vertex in 0..3 {
            let at = 20 + vertex * 24;
            raw[at + 12..at + 14].copy_from_slice(&4096i16.to_le_bytes());
            raw[at + 20..at + 24].fill(255);
        }
        let last = raw.len() - 2;
        raw[last..].copy_from_slice(&31u16.to_le_bytes());
        let (doc, png) = memory_card_icon(&raw, "icon.png").unwrap();
        validate_gltf(&doc, &json!({"schema_version":1})).unwrap();
        assert_eq!(doc["images"], json!([{"uri":"icon.png"}]));
        assert_eq!(doc["accessors"][0]["count"], 3);
        assert!(doc.get("extras").is_none());
        assert!(doc.get("animations").is_none());
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir_all(temp.path().join("raw/title")).unwrap();
        fs::create_dir_all(temp.path().join("out/data/title")).unwrap();
        fs::write(temp.path().join("raw/title/icon.ico"), &raw).unwrap();
        fs::write(temp.path().join("out/data/title/icon.ico"), &raw).unwrap();
        convert_and_strip(&temp.path().join("raw"), &temp.path().join("out"), &[]).unwrap();
        verify(&temp.path().join("out")).unwrap();
        assert!(temp.path().join("out/data/title/icon.gltf").is_file());
        let image = temp.path().join("out/data/textures/title/icon.png");
        assert_eq!(fs::read(&image).unwrap(), png);
        let published: Value = serde_json::from_slice(
            &fs::read(temp.path().join("out/data/title/icon.gltf")).unwrap(),
        )
        .unwrap();
        assert_eq!(
            published["images"],
            json!([{"uri":"../textures/title/icon.png"}])
        );
        fs::remove_file(&image).unwrap();
        assert!(verify(&temp.path().join("out")).is_err());
        fs::write(&image, &png).unwrap();
        assert!(!temp.path().join("out/data/title/icon.game.json").exists());
        assert!(!temp.path().join("out/data/title/icon.ico").exists());
        fs::write(temp.path().join("out/data/title/icon.ico"), raw).unwrap();
        assert!(verify(&temp.path().join("out")).is_err());
    }
    #[test]
    fn motion_keys_are_decoded_without_opaque_payload() {
        let mut b = vec![0; 64];
        put(&mut b, 8, 2);
        put(&mut b, 16, 1);
        put(&mut b, 32, 30);
        put(&mut b, 48, 3f32.to_bits());
        let r = Resource {
            alias: "hero.mot".into(),
            data: b,
        };
        let c = channels(&r).unwrap();
        assert_eq!(c[0]["keys"][0]["frame"], 30);
        assert_eq!(c[0]["keys"][0]["value"][0], 3.);
    }
    #[test]
    fn embedded_buffer_has_aligned_accessors() {
        let mut g = Gltf::new();
        g.joints(&[0, 1, 2, 3]);
        g.accessor(&[0., 0., 0.], 3, "VEC3", true);
        let doc = g.finish();
        assert_eq!(doc["bufferViews"][1]["byteOffset"], 8);
        assert!(
            doc["buffers"][0]["uri"]
                .as_str()
                .unwrap()
                .starts_with("data:application/octet-stream;base64,")
        );
    }
    fn test_png(colour: u8) -> Vec<u8> {
        let mut png = Vec::new();
        let mut encoder = png::Encoder::new(&mut png, 1, 1);
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder
            .write_header()
            .unwrap()
            .write_image_data(&[colour; 4])
            .unwrap();
        png
    }
    #[test]
    fn image_uris_are_relative_and_percent_encoded() {
        assert_eq!(relative_uri("a/b.gltf", "a/t/x.png"), "t/x.png");
        assert_eq!(relative_uri("a/b/c.gltf", "a/x.png"), "../x.png");
        assert_eq!(relative_uri("a/b/c.gltf", "d/e/x.png"), "../../d/e/x.png");
        assert_eq!(relative_uri("c.gltf", "d/x y#%.png"), "d/x%20y%23%25.png");
        assert_eq!(relative_uri("a/x.png/c.gltf", "a/x.png"), "../x.png");
        for (document, target) in [
            ("a/b/c.gltf", "a/d/e f.png"),
            ("c.gltf", "x.png"),
            ("a/b/c/d.gltf", "e/%41.png"),
        ] {
            let uri = relative_uri(document, target);
            assert_eq!(resolve_uri(document, &uri).unwrap(), target);
        }
        assert_eq!(resolve_uri("a/b/c.gltf", "../../x.png").unwrap(), "x.png");
        for uri in [
            "data:image/png;base64,AAAA",
            "/x.png",
            "../../x.png",
            "a//x.png",
            "./x.png",
            "%2E%2E/x.png",
            "a%2Fx.png",
            "x%2.png",
            "x.png?v=1",
            "x.png#0",
            "a\\x.png",
        ] {
            assert!(resolve_uri("a/c.gltf", uri).is_err(), "{uri}");
        }
    }
    #[test]
    fn shared_model_images_reference_standalone_pngs_externally() {
        let temp = tempfile::tempdir().unwrap();
        let port = temp.path().join("data");
        fs::create_dir_all(port.join("map/d01/tex")).unwrap();
        fs::create_dir_all(port.join("shared")).unwrap();
        let stone = test_png(1);
        let sky = test_png(2);
        fs::write(port.join("map/d01/tex/stone 0.png"), &stone).unwrap();
        fs::write(port.join("shared/sky_0.png"), &sky).unwrap();
        let mut report = ConversionReport::default();
        let documents = [
            (
                "map/d01/floor 1/room#1.gltf",
                ["../tex/stone%200.png", "../../../shared/sky_0.png"],
            ),
            (
                "map/d01/room.gltf",
                ["tex/stone%200.png", "../../shared/sky_0.png"],
            ),
        ];
        for (path, _) in documents {
            let mut gltf = json!({"asset":{"version":"2.0"},"images":[
                {"name":"stone","extras":{"chronicle_two":{"png":"map/d01/tex/stone 0.png"}}},
                {"name":"stone mip","extras":{"chronicle_two":{"png":"map/d01/tex/stone 0.png"}}},
                {"name":"sky","extras":{"chronicle_two":{"png":"shared/sky_0.png"}}}
            ]});
            let mut metadata = json!({"schema_version":1,"kind":"model"});
            publish_pair(temp.path(), path, &mut gltf, &mut metadata, &mut report).unwrap();
        }
        verify(temp.path()).unwrap();
        assert_eq!(
            report.referenced_pngs,
            BTreeSet::from(["map/d01/tex/stone 0.png".into(), "shared/sky_0.png".into()])
        );
        for (path, [stone_uri, sky_uri]) in documents {
            let text = fs::read_to_string(port.join(path)).unwrap();
            assert!(!text.contains("data:image"));
            let gltf: Value = serde_json::from_str(&text).unwrap();
            assert_eq!(
                gltf["images"],
                json!([{"name":"stone","uri":stone_uri},{"name":"stone mip","uri":stone_uri},
                       {"name":"sky","uri":sky_uri}])
            );
            for (image, expected) in [(0, &stone), (1, &stone), (2, &sky)] {
                let uri = gltf["images"][image]["uri"].as_str().unwrap();
                let resolved = resolve_uri(path, uri).unwrap();
                assert_eq!(&fs::read(port.join(resolved)).unwrap(), expected);
            }
            let sidecar: Value = serde_json::from_slice(
                &fs::read(port.join(Path::new(path).with_extension("game.json"))).unwrap(),
            )
            .unwrap();
            assert_eq!(sidecar["images"][2]["png"], "shared/sky_0.png");
        }

        let mut unbacked = json!({"asset":{"version":"2.0"},"images":[{"name":"lost"}]});
        let mut metadata = json!({"schema_version":1,"kind":"model"});
        assert!(
            publish_pair(
                temp.path(),
                "lost.gltf",
                &mut unbacked,
                &mut metadata,
                &mut report
            )
            .is_err()
        );
        let mut missing = json!({"asset":{"version":"2.0"},"images":[
            {"name":"gone","extras":{"chronicle_two":{"png":"gone.png"}}}
        ]});
        assert!(
            publish_pair(
                temp.path(),
                "lost.gltf",
                &mut missing,
                &mut metadata,
                &mut report
            )
            .is_err()
        );

        let embedded = port.join("embedded.gltf");
        save(
            &embedded,
            &json!({"asset":{"version":"2.0"},"images":[{"uri":format!("data:image/png;base64,{}",base64(&sky))}]}),
        )
        .unwrap();
        save(
            &port.join("embedded.game.json"),
            &json!({"schema_version":1}),
        )
        .unwrap();
        assert!(verify(temp.path()).is_err());
        fs::remove_file(&embedded).unwrap();
        fs::remove_file(port.join("embedded.game.json")).unwrap();
        verify(temp.path()).unwrap();
        fs::remove_file(port.join("shared/sky_0.png")).unwrap();
        assert!(verify(temp.path()).is_err());
    }
    #[test]
    fn base64_padding() {
        assert_eq!(base64(b"a"), "YQ==");
        assert_eq!(base64(b"ab"), "YWI=");
        assert_eq!(base64(b"abc"), "YWJj");
    }
    fn test_pack(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut output = Vec::new();
        for (name, payload) in entries {
            let mut header = vec![0; 80];
            header[..name.len()].copy_from_slice(name.as_bytes());
            put(&mut header, 64, 80);
            put(&mut header, 68, payload.len() as u32);
            put(&mut header, 72, (80 + payload.len()) as u32);
            output.extend(header);
            output.extend_from_slice(payload);
        }
        output.extend([0; 80]);
        output
    }
    #[test]
    fn embedded_texture_options_remain_absent_for_plain_and_canvas_pictures() {
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir_all(temp.path().join("data")).unwrap();
        let mut png = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut png, 1, 1);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            encoder
                .write_header()
                .unwrap()
                .write_image_data(&[255; 4])
                .unwrap();
        }
        fs::write(temp.path().join("data/plain.png"), png).unwrap();
        let source = super::super::textures::ConvertedTextureDocument {
            source_alias: "plain.img".into(),
            document_path: "plain.json".into(),
            document: json!({"schema_version":3,"groups":[],"entries":[{"name":"plain","pictures":[{"levels":["plain.png"]}]},{"name":"canvas","pictures":[{"canvas":{"width":1,"height":1}}]}]}),
        };
        let embedded = embed_texture_set(&mut empty_gltf(), temp.path(), &source).unwrap();
        assert!(
            embedded["document"]["entries"][0]["pictures"][0]
                .get("variants")
                .is_none()
        );
        let canvas = &embedded["document"]["entries"][1]["pictures"][0];
        assert!(canvas.get("levels").is_none());
        assert!(canvas.get("variants").is_none());
        let mut pictures = source.document["entries"][1]["pictures"].clone();
        remap_image_metadata(&mut pictures, 4);
        assert!(pictures[0].get("levels").is_none());
        assert!(pictures[0].get("variants").is_none());
    }
    fn canvas_document(source: &str) -> super::super::textures::ConvertedTextureDocument {
        super::super::textures::ConvertedTextureDocument {
            source_alias: format!("{source}.img"),
            document_path: format!("{source}.json"),
            document: json!({"schema_version":3,"groups":[],"entries":[{"name":"wave","pictures":[{"canvas":{"width":64,"height":64}}]}]}),
        }
    }
    fn picture_document(source: &str) -> super::super::textures::ConvertedTextureDocument {
        super::super::textures::ConvertedTextureDocument {
            source_alias: format!("{source}.img"),
            document_path: format!("{source}.json"),
            document: json!({"schema_version":3,"groups":[],"entries":[{"name":"WAVE","pictures":[{"levels":["wave.png"]}]}]}),
        }
    }
    /// Resolves a 96-byte material record naming `wave` for a model at `alias`.
    fn wave_material(
        alias: &str,
        documents: &[super::super::textures::ConvertedTextureDocument],
    ) -> (Gltf, Value, BTreeSet<String>) {
        let all = textures(Path::new("published"), documents).unwrap();
        let mut record = vec![0; 96];
        for at in (0..48).step_by(4) {
            record[at..at + 4].copy_from_slice(&1f32.to_le_bytes());
        }
        record[52..56].copy_from_slice(b"wave");
        let mut gltf = Gltf::new();
        let mut used = BTreeSet::new();
        let material = material(&mut gltf, &record, 0, alias, &all, &mut used).unwrap();
        (gltf, material, used)
    }
    fn assert_canvas_binds_by_name(gltf: &Gltf, material: &Value, used: &BTreeSet<String>) {
        assert_eq!(material["texture"], "wave");
        assert!(material["texture_source"].is_null());
        assert!(material["texture_path"].is_null());
        assert!(material["texture_images"].is_null());
        assert_eq!(gltf.doc["materials"][0]["name"], "wave");
        assert!(
            gltf.doc["materials"][0]["pbrMetallicRoughness"]
                .get("baseColorTexture")
                .is_none()
        );
        assert!(gltf.doc.get("images").is_none());
        assert!(gltf.doc.get("textures").is_none());
        assert!(used.is_empty());
    }
    fn assert_picture_embeds(gltf: &Gltf, material: &Value, used: &BTreeSet<String>, source: &str) {
        assert_eq!(material["texture"], "wave");
        assert_eq!(material["texture_source"], format!("{source}.img"));
        assert_eq!(
            material["texture_path"],
            format!("{}/wave.png", parent(source))
        );
        assert_eq!(material["texture_images"], json!([{"levels":[0]}]));
        assert_eq!(
            gltf.doc["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"],
            json!({"index":0})
        );
        assert_eq!(
            gltf.doc["images"][0]["extras"]["chronicle_two"]["png"],
            format!("{}/wave.png", parent(source))
        );
        assert_eq!(used, &BTreeSet::from([format!("{source}.img")]));
    }
    #[test]
    fn nearer_canvas_keeps_its_name_from_a_remote_picture_in_either_order() {
        // Veniccio's shore waves: the map's own canvas, and Luna Lab's picture of the same name.
        let alias = "map/g0301/g0301_20.mds";
        let canvas = || canvas_document("map/g0301/g0301");
        let remote = || picture_document("map/s59/s59");
        for documents in [[canvas(), remote()], [remote(), canvas()]] {
            let (gltf, material, used) = wave_material(alias, &documents);
            assert_canvas_binds_by_name(&gltf, &material, &used);
        }
    }
    #[test]
    fn nearer_picture_still_embeds_over_a_remote_canvas_in_either_order() {
        let alias = "map/g0301/g0301_20.mds";
        let local = || picture_document("map/g0301/g0301");
        let canvas = || canvas_document("map/s59/s59");
        for documents in [[local(), canvas()], [canvas(), local()]] {
            let (gltf, material, used) = wave_material(alias, &documents);
            assert_picture_embeds(&gltf, &material, &used, "map/g0301/g0301");
        }
    }
    #[test]
    fn equally_near_canvas_and_picture_resolve_to_the_first_document() {
        // Both sources share only "map/" with the model's directory, so the earlier document wins.
        let alias = "map/g0301/g0301_20.mds";
        let canvas = canvas_document("map/a01/a01");
        let picture = picture_document("map/b01/b01");
        let (gltf, material, used) = wave_material(alias, &[canvas, picture]);
        assert_canvas_binds_by_name(&gltf, &material, &used);
        let canvas = canvas_document("map/a01/a01");
        let picture = picture_document("map/b01/b01");
        let (gltf, material, used) = wave_material(alias, &[picture, canvas]);
        assert_picture_embeds(&gltf, &material, &used, "map/b01/b01");
    }

    fn test_quaternion_matrix(q: &[f32]) -> Vec<f32> {
        let (x, y, z, w) = (q[0], q[1], q[2], q[3]);
        vec![
            1. - 2. * (y * y + z * z),
            2. * (x * y + w * z),
            2. * (x * z - w * y),
            0.,
            2. * (x * y - w * z),
            1. - 2. * (x * x + z * z),
            2. * (y * z + w * x),
            0.,
            2. * (x * z + w * y),
            2. * (y * z - w * x),
            1. - 2. * (x * x + y * y),
            0.,
            0.,
            0.,
            0.,
            1.,
        ]
    }
    fn test_accessor(gltf: &Gltf, accessor: usize) -> Vec<f32> {
        gltf.accessor_floats(accessor)
    }
    #[test]
    fn nonuniform_scale_parent_preserves_actual_retail_pose_skin_and_morph_targets() {
        let raw = [0.628367, -0.021838663, 0.025334852, 0.7771976];
        let scale = [0.92420113, 0.99999994, 1.0000001];
        let mut gltf = Gltf::new();
        let mut matrix = identity();
        matrix[12..15].copy_from_slice(&[3., 4., 5.]);
        gltf.doc["nodes"] = json!([{"name":"source","matrix":matrix,"mesh":0,"skin":0,"children":[1],"weights":[0,0,0]},{"matrix":identity()}]);
        gltf.doc["scenes"][0]["nodes"] = json!([0]);
        let position = gltf.accessor(&[1., 0., 0., 0., 1., 0., 0., 0., 1.], 3, "VEC3", true);
        let joints = gltf.joints(&[0; 12]);
        let weights = gltf.accessor(
            &[1., 0., 0., 0., 1., 0., 0., 0., 1., 0., 0., 0.],
            4,
            "VEC4",
            false,
        );
        let bind = gltf.accessor(&identity(), 16, "MAT4", false);
        gltf.doc["skins"] = json!([{"joints":[0],"inverseBindMatrices":bind}]);
        gltf.doc["meshes"] = json!([{"primitives":[{"attributes":{"POSITION":position,"JOINTS_0":joints,"WEIGHTS_0":weights},"mode":4}]}]);
        let metadata = json!({"frame":0,"vertex_count":3,"primitives":[{"source_vertices":[0,1,2]}],"streams":{"positions":[[1,0,0,1],[0,1,0,1],[0,0,1,1]]}});
        let channels = vec![
            json!({"frame":0,"type":0,"keys":[{"frame":14,"value":raw}]}),
            json!({"frame":0,"type":1,"keys":[{"frame":14,"value":[scale[0],scale[1],scale[2],0]}]}),
            json!({"frame":0,"target":1,"type":12,"keys":[{"frame":14,"value":[2,0,0,1]}]}),
        ];
        prepare_morphs(&mut gltf, &[metadata], &channels).unwrap();
        prepare_transform_nodes(&mut gltf, &[&channels]).unwrap();
        assert_eq!(gltf.rotation_nodes[&0], 2);
        assert!(gltf.doc["nodes"][0].get("mesh").is_none());
        assert_eq!(gltf.doc["nodes"][2]["mesh"], 0);
        assert_eq!(gltf.doc["nodes"][2]["children"], json!([1]));
        assert_eq!(gltf.doc["nodes"][2]["weights"], json!([0, 0, 0]));
        assert_eq!(gltf.doc["skins"][0]["joints"], json!([2]));
        animation_clip(
            &mut gltf,
            "actual-p00-frame74-time14",
            &channels,
            (14, 14),
            0,
            true,
        )
        .unwrap();
        let animation = &gltf.doc["animations"][0];
        let rotation = animation["channels"]
            .as_array()
            .unwrap()
            .iter()
            .find(|channel| channel["target"]["path"] == "rotation")
            .unwrap();
        assert_eq!(rotation["target"]["node"], 2);
        let sampler = rotation["sampler"].as_u64().unwrap() as usize;
        let q = test_accessor(
            &gltf,
            animation["samplers"][sampler]["output"].as_u64().unwrap() as usize,
        );
        let scaling = animation["channels"]
            .as_array()
            .unwrap()
            .iter()
            .find(|channel| channel["target"]["path"] == "scale" && channel["target"]["node"] == 0)
            .unwrap();
        let sampler = scaling["sampler"].as_u64().unwrap() as usize;
        let actual_scale = test_accessor(
            &gltf,
            animation["samplers"][sampler]["output"].as_u64().unwrap() as usize,
        );
        assert_eq!(actual_scale, scale);
        let mut parent = identity();
        parent[0] = actual_scale[0];
        parent[5] = actual_scale[1];
        parent[10] = actual_scale[2];
        parent[12..15].copy_from_slice(&[3., 4., 5.]);
        let actual = multiply(&parent, &test_quaternion_matrix(&q));
        let expected = multiply(
            &parent,
            &test_quaternion_matrix(&[-raw[1], -raw[2], -raw[3], raw[0]]),
        );
        for (actual, expected) in actual.iter().zip(expected) {
            assert!((actual - expected).abs() < 1e-6);
        }
        assert!(
            animation["channels"]
                .as_array()
                .unwrap()
                .iter()
                .any(|channel| channel["target"]["path"] == "weights"
                    && channel["target"]["node"] == 2)
        );
        let doc = gltf.finish();
        validate_gltf(
            &doc,
            &json!({"schema_version":1,"frames":[{"mdt":0,"rotation_node":2},{"mdt":null}]}),
        )
        .unwrap();
    }

    #[test]
    fn authored_basis_scale_is_preserved_at_rest_and_reset_by_rotation_animation() {
        for scale in [[1., 0.864, 1.], [-1., 1., 1.]] {
            let mut gltf = Gltf::new();
            let mut matrix = identity();
            matrix[0] = scale[0];
            matrix[5] = scale[1];
            matrix[10] = scale[2];
            gltf.doc["nodes"] = json!([{"matrix":matrix}]);
            gltf.doc["scenes"][0]["nodes"] = json!([0]);
            let channels = vec![json!({"frame":0,"type":0,"keys":[{"frame":0,"value":[1,0,0,0]}]})];
            prepare_transform_nodes(&mut gltf, &[&channels]).unwrap();
            assert!(gltf.rotation_nodes.is_empty());
            assert_eq!(gltf.stats.plain, 1);
            animation_clip(&mut gltf, "reset", &channels, (0, 0), 0, true).unwrap();
            let animation = &gltf.doc["animations"][0];
            let reset = animation["channels"]
                .as_array()
                .unwrap()
                .iter()
                .find(|channel| channel["target"]["path"] == "scale")
                .unwrap();
            assert_eq!(reset["target"]["node"], 0);
            let sampler = reset["sampler"].as_u64().unwrap() as usize;
            assert_eq!(
                test_accessor(
                    &gltf,
                    animation["samplers"][sampler]["output"].as_u64().unwrap() as usize
                ),
                vec![1., 1., 1.]
            );
            assert!(
                (gltf.doc["nodes"][0]["scale"][0].as_f64().unwrap() - scale[0] as f64).abs() < 1e-6
            );
        }
    }
    #[test]
    fn rebaked_scale_keys_keep_the_authored_rotation_in_every_clip() {
        let mut gltf = Gltf::new();
        // Authored a quarter turn about z, as e101a's jnt21_1 is, with the uniform scale and an offset
        // event/tbox/p00_27a's c20a has.
        let scale = 1.2802608f32;
        let mut matrix = identity();
        matrix[0] = 0.;
        matrix[1] = scale;
        matrix[4] = -scale;
        matrix[5] = 0.;
        matrix[10] = scale;
        matrix[12] = 2.;
        matrix[13] = 3.;
        matrix[14] = 4.;
        gltf.doc["nodes"] = json!([{"matrix":matrix}]);
        gltf.doc["scenes"][0]["nodes"] = json!([0]);
        // Scale keys only: retail scales the authored matrix in every action.
        let channels = vec![json!({"frame":0,"type":1,"keys":[
            {"frame":0,"value":[1,1,1,0]},
            {"frame":4,"value":[1.35,1.35,1.35,0]},
            {"frame":8,"value":[1,1,1,0]},
            {"frame":12,"value":[1.35,1.35,1.35,0]}
        ]})];
        prepare_transform_nodes(&mut gltf, &[&channels]).unwrap();
        assert!(gltf.baked.contains(&0));
        animation_clip(&mut gltf, "first", &channels, (0, 4), 0, true).unwrap();
        animation_clip(&mut gltf, "second", &channels, (8, 12), 0, true).unwrap();
        assert_eq!(gltf.doc["nodes"][0]["translation"], json!([2., 3., 4.]));
        let animations = gltf.doc["animations"].as_array().unwrap();
        assert_eq!(animations.len(), 2);
        let output = |animation: &Value, path: &str| {
            let channel = animation["channels"]
                .as_array()
                .unwrap()
                .iter()
                .find(|channel| channel["target"]["path"] == path)
                .unwrap();
            let sampler = channel["sampler"].as_u64().unwrap() as usize;
            test_accessor(
                &gltf,
                animation["samplers"][sampler]["output"].as_u64().unwrap() as usize,
            )
        };
        let half = std::f32::consts::FRAC_1_SQRT_2;
        for animation in animations {
            for q in output(animation, "rotation").chunks(4) {
                let dot = q[2] * half + q[3] * half;
                assert!(
                    (dot.abs() - 1.).abs() < 1e-5,
                    "{} rotation {q:?}",
                    animation["name"]
                );
            }
            // The authored scale times the key: 1.2802608 at rest, rising to 1.728352, not 1.35.
            let scales = output(animation, "scale");
            let (first, last) = (&scales[..3], &scales[scales.len() - 3..]);
            assert!(
                first.iter().all(|value| (value - scale).abs() < 1e-5)
                    && last.iter().all(|value| (value - 1.728352).abs() < 1e-5),
                "{} scale {scales:?}",
                animation["name"]
            );
        }
    }
    #[test]
    fn scalar_first_native_rotations_become_standard_conjugate_quaternions() {
        for raw in [
            [1., 0., 0., 0.],
            [0.70710677, 0.70710677, 0., 0.],
            [0.5, 0.5, 0.5, 0.5],
        ] {
            let mut gltf = Gltf::new();
            gltf.doc["nodes"] = json!([{"matrix":identity()}]);
            animation_clip(
                &mut gltf,
                "rotation",
                &[json!({"frame":0,"type":0,"keys":[{"frame":0,"value":raw}]})],
                (0, 0),
                0,
                true,
            )
            .unwrap();
            let output = gltf.doc["animations"][0]["samplers"][0]["output"]
                .as_u64()
                .unwrap() as usize;
            let q = test_accessor(&gltf, output);
            let [w, x, y, z] = raw;
            let native = [
                1. - 2. * (y * y + z * z),
                2. * (x * y - w * z),
                2. * (x * z + w * y),
                2. * (x * y + w * z),
                1. - 2. * (x * x + z * z),
                2. * (y * z - w * x),
                2. * (x * z - w * y),
                2. * (y * z + w * x),
                1. - 2. * (x * x + y * y),
            ];
            let [x, y, z, w] = [q[0], q[1], q[2], q[3]];
            let standard = [
                1. - 2. * (y * y + z * z),
                2. * (x * y + w * z),
                2. * (x * z - w * y),
                2. * (x * y - w * z),
                1. - 2. * (x * x + z * z),
                2. * (y * z + w * x),
                2. * (x * z + w * y),
                2. * (y * z - w * x),
                1. - 2. * (x * x + y * y),
            ];
            for (expected, actual) in native.into_iter().zip(standard) {
                assert!(
                    (expected - actual).abs() < 1e-6,
                    "{raw:?}: {expected} != {actual}"
                );
            }
        }
    }

    #[test]
    fn duplicated_sibling_model_preserves_character_profiles_and_rejects_different_geometry() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir_all(&raw).unwrap();
        let mut model = vec![0; 128];
        model[..4].copy_from_slice(b"MDS\0");
        put(&mut model, 8, 1);
        put(&mut model, 12, 16);
        put(&mut model, 20, 112);
        model[24..28].copy_from_slice(b"root");
        put(&mut model, 60, u32::MAX);
        for (index, value) in identity().iter().enumerate() {
            put(&mut model, 64 + index * 4, value.to_bits());
        }
        let character = test_pack(&[("hake.mds", &model), ("info.cfg", b"MODEL \"hake.mds\";")]);
        let source = test_pack(&[("hake.mds", &model), ("hake.chr", &character)]);
        fs::write(raw.join("gsys.pak"), source).unwrap();
        let published = temp.path().join("published");
        let textures = super::super::textures::convert_tree(&raw, &published, 1).unwrap();
        convert_and_strip(&raw, &published, &textures.documents).unwrap();
        let doc: Value = serde_json::from_slice(
            &fs::read(published.join("data/gsys.pak/hake.game.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(doc["kind"], "character");
        assert_eq!(
            doc["profiles"]["info"]["commands"][0]["args"][0]["index"],
            0
        );
        assert_eq!(doc["models"].as_array().unwrap().len(), 1);
        verify(&published).unwrap();
        model[24] = b'X';
        fs::write(
            raw.join("gsys.pak"),
            test_pack(&[("hake.mds", &model), ("hake.chr", &character)]),
        )
        .unwrap();
        let other = temp.path().join("other");
        let textures = super::super::textures::convert_tree(&raw, &other, 1).unwrap();
        let error = convert_and_strip(&raw, &other, &textures.documents)
            .err()
            .unwrap();
        assert!(error.0.contains("output collision"));
    }

    #[test]
    fn compact_packs_remove_consumed_members_and_preserve_unrelated_payloads() {
        let nested = test_pack(&[("old.mds", b""), ("keep.bin", b"unchanged")]);
        let input = test_pack(&[
            ("child.chr", b"original"),
            ("old.cfg", b""),
            ("nested.pac", &nested),
        ]);
        let output = compact_pack(&input).unwrap().unwrap();
        let entries = pack(&output).unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].name, "nested.pac");
        let child = &output[entries[0].start..entries[0].start + entries[0].size];
        let entries = pack(child).unwrap();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].name, "keep.bin");
        assert_eq!(
            &child[entries[0].start..entries[0].start + entries[0].size],
            b"unchanged"
        );
        assert!(
            compact_pack(&test_pack(&[("child.chr", b"x"), ("consumed.mot", b"")]))
                .unwrap()
                .is_none()
        );
    }
    #[test]
    fn merged_nodes_do_not_gain_null_optional_standard_fields() {
        let mut destination = empty_gltf();
        merge_gltf(
            &mut destination,
            &json!({"nodes":[{"name":"root"}],"scenes":[{"nodes":[0]}]}),
        );
        assert_eq!(destination["nodes"][0], json!({"name":"root"}));
    }
    #[test]
    fn character_shadow_resources_use_bind_and_weights_slots() {
        let refs = BTreeMap::from([
            (("bind_pose".into(), "hero.chr/shadow.bbp".into()), 2),
            (("weights".into(), "hero.chr/shadow.wgt".into()), 3),
        ]);
        let profile = rewrite_profile(
            super::super::configs::parse(b"SHADOW_MOTION \"\",\"shadow.bbp\",\"shadow.wgt\";")
                .unwrap(),
            "hero.chr/info.cfg",
            "hero.chr",
            &refs,
            &mut NameBook::default(),
            &BTreeMap::new(),
        )
        .unwrap();
        assert_eq!(
            profile["commands"][0]["args"][1],
            json!({"type":"resource","kind":"bind_pose","index":2})
        );
        assert_eq!(
            profile["commands"][0]["args"][2],
            json!({"type":"resource","kind":"weights","index":3})
        );
    }
    #[test]
    fn semantic_sidecar_contains_only_stream_components_lost_by_standard_attributes() {
        let mut detail = json!({"source":"retail.mds","reserved_u32":[123],"primitives":[{"source_vertices":[0],"source_normals":[],"source_uvs":[],"source_colours":[]}],"streams":{"positions":[[1.,2.,3.,1.]],"normals":[],"uvs":[],"colours":[]}});
        prune_metadata(&mut detail);
        assert!(detail.get("source").is_none());
        assert!(detail.get("streams").is_none());
        assert!(detail.get("stream_overrides").is_none());
        assert!(detail.get("reserved_u32").is_none());
    }
    #[test]
    fn vertex_motion_uses_standard_sparse_morph_targets() {
        let mut g = Gltf::new();
        let position = g.accessor(&[0., 0., 0., 0., 0., 0., 0., 0., 0.], 3, "VEC3", true);
        g.doc["nodes"] = json!([{"mesh":0}]);
        g.doc["meshes"] = json!([{"primitives":[{"attributes":{"POSITION":position},"mode":4}]}]);
        let meshes = vec![
            json!({"frame":0,"vertex_count":1,"primitives":[{"source_vertices":[0,0,0]}],"streams":{"positions":[[0,0,0,1]]}}),
        ];
        let cs = vec![
            json!({"frame":0,"target":1,"type":12,"keys":[{"frame":0,"value":[0,0,0,1]},{"frame":30,"value":[2,3,4,1]}]}),
        ];
        prepare_morphs(&mut g, &meshes, &cs).unwrap();
        animation_clip(&mut g, "vertex", &cs, (0, 30), 0, true).unwrap();
        let doc = g.finish();
        assert_eq!(
            doc["meshes"][0]["primitives"][0]["targets"]
                .as_array()
                .unwrap()
                .len(),
            3
        );
        assert_eq!(
            doc["animations"][0]["channels"][0]["target"]["path"],
            "weights"
        );
        validate_gltf(&doc, &json!({"schema_version":1,"frames":[]})).unwrap();
    }

    #[test]
    fn incompatible_motion_is_detected_and_clipped_by_frame() {
        let cs = vec![
            json!({"frame":65,"target":0,"type":2,"keys":[{"frame":0,"value":[0,0,0,1]},{"frame":30,"value":[1,2,3,1]}]}),
        ];
        assert!(!compatible_motion(&cs, 65));
        assert!(compatible_motion(&cs, 66));
        let mut g = Gltf::new();
        for i in 0..66 {
            g.doc["nodes"]
                .as_array_mut()
                .unwrap()
                .push(json!({"name":format!("frame_{i}")}));
        }
        assert_eq!(
            animation_clip(&mut g, "e213a", &cs, (0, 30), 0, true).unwrap(),
            Some(0)
        );
        let doc = g.finish();
        assert_eq!(doc["animations"][0]["channels"][0]["target"]["node"], 65);
        validate_gltf(&doc, &json!({"schema_version":1,"frames":[]})).unwrap();
    }
    /// Builds a retail scene of chained frames, each translated one unit up from its parent.
    fn test_model(frames: usize) -> Vec<u8> {
        let mut model = vec![0; 16 + frames * 112];
        model[..4].copy_from_slice(b"MDS\0");
        put(&mut model, 8, frames as u32);
        put(&mut model, 12, 16);
        for frame in 0..frames {
            let at = 16 + frame * 112;
            put(&mut model, at + 4, 112);
            let name = format!("f{frame}");
            model[at + 8..at + 8 + name.len()].copy_from_slice(name.as_bytes());
            put(
                &mut model,
                at + 44,
                if frame == 0 {
                    u32::MAX
                } else {
                    frame as u32 - 1
                },
            );
            let mut matrix = identity();
            matrix[13] = frame as f32;
            for (i, value) in matrix.iter().enumerate() {
                put(&mut model, at + 48 + i * 4, value.to_bits());
            }
        }
        model
    }
    /// A retail channel: frame, key type and (key frame, value) pairs.
    type TestChannel<'a> = (u32, u32, &'a [(u32, [f32; 4])]);
    /// Builds a retail motion from its channels.
    fn test_motion(channels: &[TestChannel]) -> Vec<u8> {
        let mut motion = Vec::new();
        for (index, (frame, kind, keys)) in channels.iter().enumerate() {
            let mut header = vec![0; 32];
            put(&mut header, 0, *frame);
            put(&mut header, 8, *kind);
            put(&mut header, 16, keys.len() as u32);
            put(&mut header, 20, u32::from(index + 1 < channels.len()));
            motion.extend(header);
            for (at, value) in keys.iter() {
                let mut key = vec![0; 32];
                put(&mut key, 0, *at);
                for (i, v) in value.iter().enumerate() {
                    put(&mut key, 16 + i * 4, v.to_bits());
                }
                motion.extend(key);
            }
        }
        motion
    }
    fn shift_jis(text: &str) -> Vec<u8> {
        encoding_rs::SHIFT_JIS.encode(text).0.into_owned()
    }
    fn convert_packs(
        packs: &[(&str, Vec<u8>)],
    ) -> (tempfile::TempDir, CoreResult<ConversionReport>) {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir_all(&raw).unwrap();
        for (name, bytes) in packs {
            fs::write(raw.join(name), bytes).unwrap();
        }
        let published = temp.path().join("published");
        let textures = super::super::textures::convert_tree(&raw, &published, 1).unwrap();
        let report = convert_and_strip(&raw, &published, &textures.documents);
        (temp, report)
    }
    fn read_json(path: PathBuf) -> Value {
        serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
    }

    #[test]
    fn key_intervals_become_named_english_actions() {
        let motion = test_motion(&[(1, 2, &[(0, [0., 0., 0., 1.]), (30, [30., 0., 0., 1.])])]);
        let mut cfg =
            b"MODEL \"hero.mds\";MOTION 0,\"hero.mot\",\"\",\"\";KEY_START;KEY \"".to_vec();
        cfg.extend(shift_jis("立ち"));
        cfg.extend(b"\",5,7,0.02;KEY \"");
        cfg.extend(shift_jis("歩き"));
        cfg.extend(b"\",10,30,0.5;KEY_END;MOTION_END;SE_START 2;SE \"");
        cfg.extend(shift_jis("歩き"));
        cfg.extend(b"\",0,0,20.0;SE \"");
        cfg.extend(shift_jis("歩き"));
        cfg.extend(b"\",1,0,0.25;SE_END;");
        let pack = test_pack(&[
            ("hero.mds", &test_model(2)),
            ("hero.mot", &motion),
            ("info.cfg", &cfg),
        ]);
        let (temp, report) = convert_packs(&[("hero.chr", pack)]);
        assert_eq!(report.unwrap().transforms.boundary_keys, 3);
        let published = temp.path().join("published");
        verify(&published).unwrap();
        let doc = read_json(published.join("data/hero.gltf"));
        let game = read_json(published.join("data/hero.game.json"));
        let names: Vec<_> = doc["animations"]
            .as_array()
            .unwrap()
            .iter()
            .map(|a| a["name"].as_str().unwrap())
            .collect();
        assert_eq!(names, ["idle", "walk"]);
        let resource = &game["resources"]["animations"][0];
        assert_eq!(resource["actions"][0]["name"], "idle");
        assert_eq!(resource["actions"][0]["duration"], 2);
        assert_eq!(resource["actions"][1]["duration"], 20);
        assert_eq!(resource["variants"][0]["animations"], json!([0, 1]));
        assert_eq!(game["game_names"]["idle"]["value"], "立ち");
        assert_eq!(game["game_names"]["walk"]["value"], "歩き");
        let commands = game["profiles"]["info"]["commands"].as_array().unwrap();
        let key = commands.iter().find(|c| c["command"] == "KEY").unwrap();
        assert_eq!(key["args"].as_array().unwrap().len(), 2);
        assert_eq!(key["args"][0]["value"], "idle");
        let sounds: Vec<_> = commands.iter().filter(|c| c["command"] == "SE").collect();
        assert_eq!(sounds[0]["args"][0]["value"], "walk");
        assert_eq!(sounds[0]["args"][3]["value"], 0.5);
        assert_eq!(sounds[1]["args"][3]["value"], 0.25);
        let idle = &doc["animations"][0];
        let input = &doc["accessors"][idle["samplers"][0]["input"].as_u64().unwrap() as usize];
        assert_eq!(input["min"][0], 0.);
        assert!((input["max"][0].as_f64().unwrap() - 2. / 30.).abs() < 1e-6);
    }

    #[test]
    fn untranslated_motion_names_fail_conversion() {
        let mut cfg =
            b"MODEL \"hero.mds\";MOTION 0,\"hero.mot\",\"\",\"\";KEY_START;KEY \"".to_vec();
        cfg.extend(shift_jis("未知の動き"));
        cfg.extend(b"\",0,1,0.5;KEY_END;");
        let motion = test_motion(&[(0, 2, &[(0, [0., 0., 0., 1.])])]);
        let pack = test_pack(&[
            ("hero.mds", &test_model(1)),
            ("hero.mot", &motion),
            ("info.cfg", &cfg),
        ]);
        let (_temp, report) = convert_packs(&[("hero.chr", pack)]);
        let error = report.err().unwrap();
        assert!(error.0.contains("no English translation"), "{}", error.0);
    }

    #[test]
    fn external_motions_are_published_into_the_models_scripts_load_them_on() {
        use super::super::native_scripts::{ScriptArg, event_script_fixture};
        let hero_motion = test_motion(&[(1, 2, &[(0, [0., 1., 0., 1.])])]);
        let character = |model: &str| {
            test_pack(&[
                (&format!("{model}.mds"), &test_model(3)),
                (&format!("{model}.mot"), &hero_motion),
                (
                    "info.cfg",
                    format!("MODEL \"{model}.mds\";MOTION 0,\"{model}.mot\",\"\",\"\";").as_bytes(),
                ),
            ])
        };
        let extension = test_motion(&[
            (1, 2, &[(0, [0., 1., 0., 1.]), (10, [0., 1., 0., 1.])]),
            (2, 2, &[(0, [0., 2., 0., 1.])]),
            (
                2,
                0,
                &[
                    (0, [1., 0., 0., 0.]),
                    (10, [0.70710677, 0.70710677, 0., 0.]),
                ],
            ),
        ]);
        let mut cfg = b"MOTION 1,\"walker.mot\",\"\",\"\";KEY_START;KEY \"".to_vec();
        cfg.extend(shift_jis("歩き"));
        cfg.extend(b"\",0,10,0.5;KEY_END;");
        let walker = test_pack(&[("walker.mot", &extension), ("info.cfg", &cfg)]);
        let stranger = test_pack(&[
            ("stranger.mot", &extension),
            ("info.cfg", b"MOTION 1,\"stranger.mot\",\"\",\"\";"),
        ]);
        // The event loads hero into actor 9 and then walker's motions into the same actor.
        let script = event_script_fixture(&[
            (
                9,
                &[
                    ScriptArg::Int(0),
                    ScriptArg::Text("hero.chr"),
                    ScriptArg::Text("info.cfg"),
                    ScriptArg::Int(9),
                ],
            ),
            (
                19,
                &[
                    ScriptArg::Int(0),
                    ScriptArg::Text("walker.chr"),
                    ScriptArg::Text("info.cfg"),
                    ScriptArg::Int(9),
                ],
            ),
        ]);
        let (temp, report) = convert_packs(&[
            ("hero.chr", character("hero")),
            ("twin.chr", character("twin")),
            ("walker.chr", walker),
            ("stranger.chr", stranger),
            ("event.stb", script),
        ]);
        let report = report.unwrap();
        // The twin shares hero's skeleton, but nothing plays walker's motions on it.
        assert_eq!(report.placements.len(), 2);
        let walker_placement = report
            .placements
            .iter()
            .find(|p| p["motion"] == "walker.gltf#0")
            .unwrap();
        assert_eq!(walker_placement["method"], "linked");
        assert_eq!(walker_placement["targets"][0]["model"], "hero.gltf");
        assert_eq!(walker_placement["targets"].as_array().unwrap().len(), 1);
        assert_eq!(report.unresolved.len(), 1);
        assert!(
            report.unresolved[0].starts_with("stranger.gltf#0: no script or game code loads it")
        );
        let published = temp.path().join("published");
        verify(&published).unwrap();
        let walker_doc = read_json(published.join("data/walker.gltf"));
        assert!(walker_doc.get("animations").is_none());
        let walker_game = read_json(published.join("data/walker.game.json"));
        let resource = &walker_game["resources"]["animations"][0];
        assert_eq!(resource["external"], "walker.gltf#0");
        assert_eq!(resource["targets"], json!(["hero.gltf"]));
        assert_eq!(resource["actions"][0]["name"], "walk");
        let hero_doc = read_json(published.join("data/hero.gltf"));
        assert_eq!(hero_doc["buffers"][0]["uri"], "hero.gltf.bin");
        let clip = hero_doc["animations"]
            .as_array()
            .unwrap()
            .iter()
            .position(|a| a["name"] == "walker/walk")
            .unwrap();
        let hero_game = read_json(published.join("data/hero.game.json"));
        let entry = hero_game["resources"]["animations"]
            .as_array()
            .unwrap()
            .iter()
            .find(|a| a["external"] == "walker.gltf#0")
            .unwrap();
        assert_eq!(entry["variants"][0]["animations"], json!([clip]));
        assert_eq!(entry["variants"][0]["model"], 0);
        let twin_doc = read_json(published.join("data/twin.gltf"));
        assert!(
            !twin_doc["animations"]
                .as_array()
                .unwrap()
                .iter()
                .any(|a| a["name"] == "walker/walk")
        );
        // A glTF that only carries clips is rejected.
        let port = published.join("data");
        let mut animation_only = walker_doc.clone();
        for key in ["animations", "accessors", "bufferViews", "nodes"] {
            animation_only[key] = hero_doc[key].clone();
        }
        animation_only["buffers"] = hero_doc["buffers"].clone();
        animation_only["buffers"][0]["uri"] = json!("walker.gltf.bin");
        fs::copy(port.join("hero.gltf.bin"), port.join("walker.gltf.bin")).unwrap();
        save(&port.join("walker.gltf"), &animation_only).unwrap();
        let error = verify(&published).err().unwrap();
        assert!(error.0.contains("animation-only glTF"), "{}", error.0);
        // A buffer file whose document is gone is rejected.
        save(&port.join("walker.gltf"), &walker_doc).unwrap();
        let error = verify(&published).err().unwrap();
        assert!(
            error.0.contains("glTF buffer without its document"),
            "{}",
            error.0
        );
        fs::remove_file(port.join("walker.gltf.bin")).unwrap();
        verify(&published).unwrap();
        // A truncated buffer file is rejected.
        fs::write(port.join("hero.gltf.bin"), b"abcd").unwrap();
        assert!(verify(&published).is_err());
    }

    #[test]
    fn transform_plans_prefer_plain_then_rebaked_then_helper() {
        let rotation = |degrees: f32| {
            let half = degrees.to_radians() / 2.;
            json!([half.cos(), 0., 0., -half.sin()])
        };
        let keys = |value: Value| {
            vec![
                json!({"frame":0,"value":value}),
                json!({"frame":4,"value":value}),
            ]
        };
        let identity = identity();
        let uniform = keys(json!([2., 2., 2., 0.]));
        let stretched = keys(json!([2., 1., 1., 0.]));
        let quarter = keys(rotation(90.));
        let eighth = keys(rotation(45.));
        assert!(matches!(
            plan_frame(&identity, Some(&quarter), Some(&uniform)),
            FramePlan::Plain
        ));
        assert!(matches!(
            plan_frame(&identity, Some(&quarter), Some(&stretched)),
            FramePlan::Baked
        ));
        assert!(matches!(
            plan_frame(&identity, Some(&eighth), Some(&stretched)),
            FramePlan::Helper(_)
        ));
        let mut gltf = Gltf::new();
        gltf.doc["nodes"] = json!([{"matrix":identity}]);
        let channels = vec![
            json!({"frame":0,"type":0,"keys":quarter}),
            json!({"frame":0,"type":1,"keys":stretched}),
        ];
        prepare_transform_nodes(&mut gltf, &[&channels]).unwrap();
        assert_eq!(gltf.stats.baked, 1);
        assert!(gltf.rotation_nodes.is_empty());
        animation_clip(&mut gltf, "baked", &channels, (0, 4), 0, false).unwrap();
        let animation = &gltf.doc["animations"][0];
        let output = |path: &str| {
            let channel = animation["channels"]
                .as_array()
                .unwrap()
                .iter()
                .find(|c| c["target"]["path"] == path)
                .unwrap();
            let sampler = channel["sampler"].as_u64().unwrap() as usize;
            test_accessor(
                &gltf,
                animation["samplers"][sampler]["output"].as_u64().unwrap() as usize,
            )
        };
        let (q, s) = (output("rotation"), output("scale"));
        let standard = product(&quaternion_matrix(&q[..4]), &diagonal(&s[..3]));
        let retail = retail_basis(&basis(&identity), Some(&quarter), &stretched, 0.);
        assert!(difference(&standard, &retail) < 1e-5);
        assert!(gltf.stats.baked_error < 1e-4);
    }

    #[test]
    fn direct_motion_payload_is_substituted_and_audited() {
        let temp = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let raw = temp.path().join("raw");
        let output = temp.path().join("out");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(output.join("data")).unwrap();
        let mut b = vec![0; 64];
        put(&mut b, 8, 2);
        put(&mut b, 16, 1);
        put(&mut b, 48, 1f32.to_bits());
        fs::write(raw.join("sample.mot"), &b).unwrap();
        fs::write(output.join("data/sample.mot"), &b).unwrap();
        convert_and_strip(&raw, &output, &[]).unwrap();
        assert!(!output.join("data/sample.mot").exists());
        verify(&output).unwrap();
        fs::write(output.join("data/sample.mot"), b).unwrap();
        assert!(verify(&output).is_err());
    }

    #[test]
    #[ignore = "requires a local retail model PACK path in KZ_MODEL_INPUT"]
    fn retail_pack_smoke() {
        let input = PathBuf::from(std::env::var("KZ_MODEL_INPUT").expect("KZ_MODEL_INPUT"));
        let temp = tempfile::tempdir_in(std::env::current_dir().unwrap()).unwrap();
        let raw = temp.path().join("raw");
        let published = temp.path().join("published");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(published.join("data")).unwrap();
        let b = fs::read(&input).unwrap();
        fs::write(raw.join("fixture.chr"), &b).unwrap();
        fs::write(published.join("data/fixture.chr"), b).unwrap();
        convert_and_strip(&raw, &published, &[]).unwrap();
        verify(&published).unwrap();
        let doc: Value =
            serde_json::from_slice(&fs::read(published.join("data/fixture.game.json")).unwrap())
                .unwrap();
        assert!(!doc["models"].as_array().unwrap().is_empty());
        assert!(!published.join("data/fixture.chr").exists());
        if let Some(path) = std::env::var_os("KZ_MODEL_FIXTURE_OUTPUT") {
            fs::copy(published.join("data/fixture.gltf"), &path).unwrap();
            fs::copy(
                published.join("data/fixture.game.json"),
                Path::new(&path).with_extension("game.json"),
            )
            .unwrap();
        }
    }

    #[test]
    #[ignore = "requires extracted retail data in KZ_MODEL_RAW and isolated KZ_MODEL_OUTPUT"]
    fn retail_transform_preflight() {
        let raw = PathBuf::from(std::env::var("KZ_MODEL_RAW").expect("KZ_MODEL_RAW"));
        let output = PathBuf::from(std::env::var("KZ_MODEL_OUTPUT").expect("KZ_MODEL_OUTPUT"));
        let report = convert_and_strip(&raw, &output, &[]).unwrap();
        let mut counts =
            json!({"assets":0,"models":0,"animations":0,"variants":0,"rotation_helpers":0});
        let add = |counts: &mut Value, key: &str, value: u64| {
            counts[key] = json!(counts[key].as_u64().unwrap() + value);
        };
        for file in report.files.iter().filter(|file| file.ends_with(".gltf")) {
            let path = output.join("data").join(file);
            let doc: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
            add(&mut counts, "assets", 1);
            if !path.with_extension("game.json").is_file() {
                continue;
            }
            let metadata: Value =
                serde_json::from_slice(&fs::read(path.with_extension("game.json")).unwrap())
                    .unwrap();
            let buffers = load_buffers(&output.join("data"), file, &doc).unwrap();
            validate_gltf_buffers(&doc, &metadata, &buffers)
                .unwrap_or_else(|error| panic!("{file}: {error}"));
            add(
                &mut counts,
                "animations",
                doc["animations"].as_array().map_or(0, Vec::len) as u64,
            );
            let models: Vec<_> = if metadata["kind"] == "character" {
                metadata["models"].as_array().unwrap().iter().collect()
            } else {
                vec![&metadata]
            };
            for model in models {
                validate_gltf_buffers(&doc, model, &buffers)
                    .unwrap_or_else(|error| panic!("{file}: {error}"));
                add(
                    &mut counts,
                    "models",
                    u64::from(model["frames"].as_array().is_some_and(|f| !f.is_empty())),
                );
                add(
                    &mut counts,
                    "rotation_helpers",
                    model["frames"]
                        .as_array()
                        .into_iter()
                        .flatten()
                        .filter(|frame| frame.get("rotation_node").is_some())
                        .count() as u64,
                );
            }
            for resource in metadata["resources"]["animations"]
                .as_array()
                .into_iter()
                .flatten()
            {
                for variant in resource["variants"].as_array().into_iter().flatten() {
                    add(&mut counts, "variants", 1);
                    validate_animation_variant(&doc, &metadata, resource, variant)
                        .unwrap_or_else(|error| panic!("{file}: {error}"));
                }
            }
        }
        save(&output.join("transform-audit.json"), &counts).unwrap();
        println!("{}", serde_json::to_string_pretty(&counts).unwrap());
    }

    #[test]
    #[ignore = "requires extracted retail data in KZ_MODEL_RAW and isolated KZ_MODEL_OUTPUT"]
    fn retail_external_placement() {
        let raw = PathBuf::from(std::env::var("KZ_MODEL_RAW").expect("KZ_MODEL_RAW"));
        let output = PathBuf::from(std::env::var("KZ_MODEL_OUTPUT").expect("KZ_MODEL_OUTPUT"));
        // Placement needs no textures; the models are converted without them.
        let report = convert_tree(&raw, &output, &[], &Phase::noop()).unwrap();
        save(
            &output.join("external-placement.json"),
            &json!({"placements":report.placements,"unresolved":report.unresolved}),
        )
        .unwrap();
    }

    #[test]
    #[ignore = "requires extracted retail data in KZ_MODEL_RAW and isolated KZ_MODEL_OUTPUT"]
    fn retail_tree_audit() {
        let raw = PathBuf::from(std::env::var("KZ_MODEL_RAW").expect("KZ_MODEL_RAW"));
        let output = PathBuf::from(std::env::var("KZ_MODEL_OUTPUT").expect("KZ_MODEL_OUTPUT"));
        if std::env::var_os("KZ_MODEL_VERIFY_ONLY").is_some() {
            verify(&output).unwrap();
            return;
        }
        let converted = super::super::textures::convert_tree(&raw, &output, 4).unwrap();
        let report = convert_and_strip(&raw, &output, &converted.documents).unwrap();
        save(
            &output.join("external-placement.json"),
            &json!({"placements":report.placements,"unresolved":report.unresolved}),
        )
        .unwrap();
        verify(&output).unwrap();
    }
}
