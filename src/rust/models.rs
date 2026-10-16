//! Converts retail scene, mesh, skeleton and animation records into glTF and typed JSON.
use super::{CoreError, CoreResult};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

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
        .chunks_exact(4)
        .map(|x| {
            let f = f32::from_le_bytes(x.try_into().unwrap());
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
fn save(path: &Path, document: &Value) -> CoreResult<()> {
    fs::create_dir_all(path.parent().ok_or_else(|| fail("missing model parent"))?)?;
    fs::write(path, serde_json::to_vec_pretty(document)?)?;
    Ok(())
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
    rotation_nodes: BTreeMap<usize, usize>,
    reset_rotation_scale: BTreeSet<usize>,
}
/// Converts a memory card icon model into glTF referencing `image_uri` and the PNG it names.
fn memory_card_icon(data: &[u8], image_uri: &str) -> CoreResult<(Value, Vec<u8>)> {
    let shapes = u32le(data, 4)? as usize;
    let count = u32le(data, 16)? as usize;
    if u32le(data, 0)? != 0x10000
        || shapes != 1
        || count == 0
        || count % 3 != 0
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

#[derive(Clone)]
struct Texture {
    source: String,
    name: String,
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
                    sampler: entry["pictures"][0]["sampler"].clone(),
                    asset: png_identity(&Path::new(&document).parent().unwrap().join(level)),
                    pictures: if entry["pictures"].is_array() {
                        entry["pictures"].clone()
                    } else {
                        json!([{"levels":entry["variants"][0]["levels"]}])
                    },
                    directory: published
                        .join("port_data")
                        .join(Path::new(&document).parent().unwrap()),
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
    let selected_texture = resolve_texture(all, alias, &texture)?;
    let mut embedded_pictures = Value::Null;
    if let Some(t) = selected_texture {
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
            images.push(json!({"name":image_name,"extras":{"chronicle_two":{"png":t.asset}}}));
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
        samplers.push(json!({"wrapS":if t.sampler["wrap_u"]=="clamp"{33071}else{10497},"wrapT":if t.sampler["wrap_v"]=="clamp"{33071}else{10497}}));
        gltf.doc["textures"]
            .as_array_mut()
            .unwrap()
            .push(json!({"source":image,"sampler":sampler}));
        pbr["baseColorTexture"] = json!({"index":id});
        let mut pictures = t.pictures.clone();
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
                            &t.directory.join(reference),
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
                            &t.directory.join(reference),
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
        json!({"texture":texture,"texture_source":selected_texture.map(|t|&t.source),"texture_path":selected_texture.map(|t|&t.asset),"texture_images":embedded_pictures,"diffuse":diffuse,"ambient":ambient,"specular":specular,"power":power,"flags":u32le(b,at+84)?,"extra":floats(b,at+88,2)?}),
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
/// Builds the relative URI from a port_data document to another port_data file.
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
/// Points every glTF image at its standalone PNG relative to the glTF file.
fn link_images(
    published: &Path,
    path: &str,
    gltf: &mut Value,
    report: &mut ConversionReport,
) -> CoreResult<()> {
    let port = published.join("port_data");
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
        if !port.join(&png).is_file() {
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
        .rsplit_once("/port_data/")
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
fn publish_pair(
    published: &Path,
    path: &str,
    gltf: &mut Value,
    metadata: &mut Value,
    report: &mut ConversionReport,
) -> CoreResult<()> {
    if metadata["kind"] == "character" {
        for model in metadata["models"].as_array_mut().unwrap() {
            model.as_object_mut().unwrap().remove("name");
            for key in ["motion_channels", "bind_poses", "weights", "schema_version"] {
                model.as_object_mut().unwrap().remove(key);
            }
        }
        for clip in metadata["resources"]["animations"].as_array_mut().unwrap() {
            if !clip["animation"].is_null() {
                clip["channels"]
                    .as_array_mut()
                    .unwrap()
                    .retain(|c| c["type"].as_i64().unwrap() > 2);
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
    link_images(published, path, gltf, report)?;
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
    save(&published.join("port_data").join(path), gltf)?;
    let sidecar = Path::new(path).with_extension("game.json");
    save(&published.join("port_data").join(&sidecar), metadata)?;
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
        for k in 0..8 {
            a[c][k] /= d
        }
        for r in 0..4 {
            if r != c {
                let f = a[r][c];
                for k in 0..8 {
                    a[r][k] -= f * a[c][k]
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
    let q;
    if trace > 0. {
        let s = (trace + 1.).sqrt() * 2.;
        q = vec![
            (r[5] - r[7]) / s,
            (r[6] - r[2]) / s,
            (r[1] - r[3]) / s,
            s / 4.,
        ];
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
        q = v;
    }
    let norm = q.iter().map(|v| v * v).sum::<f32>().sqrt();
    let q = if norm > 1e-20 {
        q.iter().map(|v| v / norm).collect::<Vec<_>>()
    } else {
        vec![0., 0., 0., 1.]
    };
    json!({"translation":&m[12..15],"rotation":q,"scale":scale})
}

fn prepare_transform_nodes(gltf: &mut Gltf, channels: &[Value]) -> CoreResult<()> {
    let mut animated = BTreeMap::<usize, (bool, bool, bool)>::new();
    for channel in channels {
        let Some(frame) = channel["frame"].as_u64().map(|value| value as usize) else {
            continue;
        };
        if !channel["keys"]
            .as_array()
            .is_some_and(|keys| !keys.is_empty())
        {
            continue;
        }
        if !matches!(channel["type"].as_i64(), Some(0 | 1)) {
            continue;
        }
        let flags = animated.entry(frame).or_default();
        match channel["type"].as_i64() {
            Some(0) => flags.0 = true,
            Some(1) => {
                flags.1 = true;
                flags.2 |= channel["keys"].as_array().unwrap().iter().any(|key| {
                    let value = &key["value"];
                    (value[0].as_f64().unwrap() - value[1].as_f64().unwrap()).abs() > 1e-5
                        || (value[0].as_f64().unwrap() - value[2].as_f64().unwrap()).abs() > 1e-5
                });
            }
            _ => {}
        }
    }
    for (frame, (rotation, scale, nonuniform)) in animated {
        if frame >= gltf.doc["nodes"].as_array().unwrap().len() {
            return Err(fail("transform animation frame outside skeleton"));
        }
        let matrix: Vec<f32> = gltf.doc["nodes"][frame]
            .get("matrix")
            .and_then(Value::as_array)
            .map(|values| {
                values
                    .iter()
                    .map(|value| value.as_f64().unwrap() as f32)
                    .collect()
            })
            .unwrap_or_else(identity);
        let authored_scale = trs(&matrix)["scale"]
            .as_array()
            .unwrap()
            .iter()
            .any(|value| (value.as_f64().unwrap() - 1.).abs() > 1e-5);
        if !nonuniform && !(authored_scale && (rotation || scale)) {
            continue;
        }
        let helper = gltf.doc["nodes"].as_array().unwrap().len();
        let mut basis = matrix.clone();
        basis[12..15].fill(0.);
        let mut child = json!({"matrix":basis});
        let node = &mut gltf.doc["nodes"][frame];
        if let Some(name) = node["name"].as_str() {
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
        if rotation && authored_scale {
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
fn rotation_map(gltf: &Gltf) -> Value {
    Value::Object(
        gltf.rotation_nodes
            .iter()
            .map(|(frame, helper)| (helper.to_string(), json!(frame)))
            .collect(),
    )
}

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
fn animation(gltf: &mut Gltf, alias: &str, channels: &[Value]) -> CoreResult<Option<usize>> {
    let mut samplers = Vec::new();
    let mut targets = Vec::new();
    let node_count = gltf.doc["nodes"].as_array().unwrap().len();
    for (channel_index, c) in channels.iter().enumerate() {
        let kind = c["type"].as_i64().unwrap();
        if !(0..=2).contains(&kind) {
            continue;
        }
        if channels[channel_index + 1..]
            .iter()
            .any(|later| later["frame"] == c["frame"] && later["type"] == c["type"])
        {
            continue;
        }
        let node = c["frame"].as_i64().unwrap();
        if node < 0 || node as usize >= node_count {
            return Err(fail(format!("motion frame {node} outside model {alias}")));
        }
        let frame = node as usize;
        let node = if kind == 0 {
            gltf.rotation_nodes.get(&frame).copied().unwrap_or(frame)
        } else {
            frame
        };
        if let Some(matrix) = gltf.doc["nodes"][node as usize]["matrix"].as_array() {
            let matrix: Vec<f32> = matrix.iter().map(|v| v.as_f64().unwrap() as f32).collect();
            let decomposed = trs(&matrix);
            let object = gltf.doc["nodes"][node as usize].as_object_mut().unwrap();
            object.remove("matrix");
            for (key, value) in decomposed.as_object().unwrap() {
                object.insert(key.clone(), value.clone());
            }
        }
        let keys = c["keys"].as_array().unwrap();
        if keys.is_empty() {
            continue;
        }
        let width = if kind == 0 { 4 } else { 3 };
        let mut time = Vec::new();
        let mut values = Vec::new();
        let mut previous = None;
        for k in keys {
            let f = k["frame"].as_u64().unwrap();
            if previous.is_some_and(|v| v >= f) {
                return Err(fail(format!("non-increasing motion keys: {alias}")));
            }
            previous = Some(f);
            time.push(f as f32 / 30.);
            let mut value: Vec<f32> = (0..width)
                .map(|i| k["value"][i].as_f64().unwrap() as f32)
                .collect();
            if kind == 0 {
                value = vec![-value[1], -value[2], -value[3], value[0]];
                let norm = value.iter().map(|v| v * v).sum::<f32>().sqrt();
                if norm > 1e-20 {
                    for v in &mut value {
                        *v /= norm
                    }
                } else {
                    value = vec![0., 0., 0., 1.];
                }
            }
            values.extend(value);
        }
        let input = gltf.accessor(&time, 1, "SCALAR", true);
        let output = gltf.accessor(
            &values,
            width,
            if width == 4 { "VEC4" } else { "VEC3" },
            false,
        );
        let sampler = samplers.len();
        samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
        targets.push(json!({"sampler":sampler,"target":{"node":node,"path":if kind==0{"rotation"}else if kind==1{"scale"}else{"translation"}}}));
        if kind == 0 && gltf.reset_rotation_scale.contains(&frame) {
            let output = gltf.accessor(&vec![1.; keys.len() * 3], 3, "VEC3", false);
            let sampler = samplers.len();
            samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
            targets.push(json!({"sampler":sampler,"target":{"node":node,"path":"scale"}}));
        }
    }
    let driven: BTreeSet<_> = channels
        .iter()
        .filter(|c| c["type"] == 12)
        .filter_map(|c| c["frame"].as_u64())
        .collect();
    for node in driven {
        let frame = node as usize;
        let node = gltf.rotation_nodes.get(&frame).copied().unwrap_or(frame);
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
        let times: BTreeSet<_> = vertex_channels
            .iter()
            .flat_map(|c| c["keys"].as_array().unwrap())
            .filter_map(|k| k["frame"].as_u64())
            .collect();
        if times.is_empty() {
            continue;
        }
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
        let input = gltf.accessor(
            &times.iter().map(|&t| t as f32 / 30.).collect::<Vec<_>>(),
            1,
            "SCALAR",
            true,
        );
        let output = gltf.accessor(&values, 1, "SCALAR", false);
        let sampler = samplers.len();
        samplers.push(json!({"input":input,"output":output,"interpolation":"LINEAR"}));
        targets.push(json!({"sampler":sampler,"target":{"node":node,"path":"weights"}}));
    }
    if targets.is_empty() {
        return Ok(None);
    }
    let animations = gltf.doc["animations"].as_array_mut().unwrap();
    let id = animations.len();
    animations.push(json!({"name":alias,"samplers":samplers,"channels":targets}));
    Ok(Some(id))
}

struct NativeArtifact {
    gltf: Value,
    metadata: Value,
    animations: Vec<Value>,
}

/// Describes native assets and texture containers consumed by model publication.
#[derive(Default)]
pub struct ConversionReport {
    pub files: Vec<String>,
    pub consumed_texture_sources: BTreeSet<String>,
    /// Port-relative standalone PNGs referenced by published glTF images.
    pub referenced_pngs: BTreeSet<String>,
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
fn shifted_rotation_map(value: &Value, offset: usize) -> Value {
    Value::Object(
        value
            .as_object()
            .into_iter()
            .flatten()
            .map(|(node, frame)| {
                (
                    (node.parse::<usize>().unwrap() + offset).to_string(),
                    frame.clone(),
                )
            })
            .collect(),
    )
}
fn merge_rotation_map(target: &mut Value, source: &Value, offset: usize) {
    if source.as_object().is_some_and(|map| !map.is_empty()) {
        let map = shifted_rotation_map(source, offset);
        let destination = target
            .as_object_mut()
            .unwrap()
            .entry("rotation_nodes")
            .or_insert_with(|| json!({}));
        destination
            .as_object_mut()
            .unwrap()
            .extend(map.as_object().unwrap().clone());
    }
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
        .join("port_data")
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
fn rewrite_profile(
    mut document: Value,
    container: &str,
    references: &BTreeMap<(String, String), usize>,
) -> CoreResult<Value> {
    let mut effect_container = container.to_owned();
    for statement in document["commands"]
        .as_array_mut()
        .ok_or_else(|| fail("character profile commands missing"))?
    {
        let command = statement["command"].as_str().unwrap_or("").to_owned();
        for (index, arg) in statement["args"]
            .as_array_mut()
            .ok_or_else(|| fail("character arguments missing"))?
            .iter_mut()
            .enumerate()
        {
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

fn publish_native(
    published: &Path,
    resources: &[Resource],
    artifacts: &BTreeMap<String, NativeArtifact>,
    documents: &[super::textures::ConvertedTextureDocument],
    converted: &mut BTreeSet<String>,
    report: &mut ConversionReport,
) -> CoreResult<()> {
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
                models.push(metadata);
                Some(id)
            } else {
                None
            };
            for animation in &artifact.animations {
                let Some(name) = animation["alias"].as_str() else {
                    continue;
                };
                if let Some(index) = animation["index"].as_u64() {
                    let index = index as usize + offsets["animations"];
                    if gltf["animations"]
                        .as_array()
                        .is_some_and(|a| index < a.len())
                    {
                        gltf["animations"][index]["name"] = json!(leaf_stem(name));
                        let mut variant = json!({"model":model_id,"animation":index,"node_offset":offsets["nodes"]});
                        if model_id.is_none() {
                            merge_rotation_map(
                                &mut variant,
                                &artifact.metadata["rotation_nodes"],
                                offsets["nodes"],
                            );
                        }
                        animation_variants
                            .entry(name.into())
                            .or_default()
                            .push(variant);
                    }
                }
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
            let mut value = json!({"name":leaf_stem(&resource.alias)});
            match kind {
                "animation" => {
                    let variants = animation_variants
                        .remove(&resource.alias)
                        .unwrap_or_default();
                    value["animation"] = variants
                        .first()
                        .map_or(Value::Null, |v| v["animation"].clone());
                    value["variants"] = json!(variants);
                    value["channels"] = json!(channels(resource)?);
                }
                "bind_pose" => value["matrices"] = bind_pose(resource)?["matrices"].clone(),
                "weights" => value["channels"] = json!(channels(resource)?),
                "cloth" => {
                    value["commands"] = super::configs::parse(&resource.data)?["commands"].clone()
                }
                "script" => {
                    let path = super::configs::native_path(&resource.alias, "stb");
                    let dest = published.join("port_data").join(&path);
                    fs::create_dir_all(dest.parent().unwrap())?;
                    fs::write(dest, &resource.data)?;
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
            converted.insert(resource.alias.clone());
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
                    if let Some(source) = material["texture_source"].as_str() {
                        if let Some(index) = references.get(&("texture_set".into(), source.into()))
                        {
                            dependencies.insert(*index);
                        }
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
            if !references.contains_key(&key) {
                let index = native["resources"]["scripts"].as_array().unwrap().len();
                native["resources"]["scripts"].as_array_mut().unwrap().push(json!({"name":leaf_stem(&resource.alias),"path":super::configs::native_path(&resource.alias,"stb"),"character":super::configs::native_path(character_parent(&resource.alias).unwrap(),"gltf")}));
                references.insert(key, index);
            }
        }
        for profile in resources.iter().filter(|r| {
            extension(&r.alias) == "cfg" && character_parent(&r.alias) == Some(alias.as_str())
        }) {
            let name = leaf_stem(&profile.alias);
            if native.get("default_profile").is_none() {
                native["default_profile"] = json!(name);
            }
            native["profiles"][&name] =
                rewrite_profile(super::configs::parse(&profile.data)?, alias, &references)?;
            converted.insert(profile.alias.clone());
        }
        let path = super::configs::native_path(alias, "gltf");
        publish_pair(published, &path, &mut gltf, &mut native, report)?;
        converted.insert(alias.clone());
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
            if offsets["nodes"] != 0
                && artifact.metadata["frames"]
                    .as_array()
                    .is_none_or(Vec::is_empty)
            {
                for animation in 0..artifact.gltf["animations"].as_array().map_or(0, Vec::len) {
                    let map = metadata
                        .as_object_mut()
                        .unwrap()
                        .entry("animation_node_offsets")
                        .or_insert_with(|| json!({}));
                    map[(animation + offsets["animations"]).to_string()] = json!(offsets["nodes"]);
                }
            }
            merge_rotation_map(
                &mut metadata,
                &artifact.metadata["rotation_nodes"],
                offsets["nodes"],
            );
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
        publish_pair(published, &path, &mut gltf, &mut metadata, report)?;
    }
    Ok(())
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

/// Publishes all model resources and removes their proprietary payloads from the compatibility tree.
pub fn convert_tree(
    raw: &Path,
    published: &Path,
    _jobs: usize,
    documents: &[super::textures::ConvertedTextureDocument],
) -> CoreResult<ConversionReport> {
    let mut report = ConversionReport::default();
    let mut files = Vec::new();
    collect(raw, &mut files)?;
    files.sort_by_key(|p| {
        (
            !matches!(p.extension().and_then(|e| e.to_str()), Some("mds" | "mdt")),
            p.clone(),
        )
    });
    let all_textures = textures(published, documents)?;
    let mut used_textures = BTreeSet::new();
    let mut catalog_resources = BTreeMap::<String, Value>::new();
    let mut converted = BTreeSet::new();
    for p in &files {
        let alias = p
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        if fs::metadata(p)?.len() > 128 << 20 {
            continue;
        }
        if converted.contains(&alias) {
            continue;
        }
        let mut artifacts = BTreeMap::<String, NativeArtifact>::new();
        let mut resources = Vec::new();
        discover(&fs::read(p)?, &alias, &mut resources, 0)?;
        for icon in resources
            .iter()
            .filter(|resource| extension(&resource.alias).eq_ignore_ascii_case("ico"))
        {
            let path = super::configs::native_path(&icon.alias, "gltf");
            let image = Path::new(&path)
                .with_extension("png")
                .to_string_lossy()
                .replace('\\', "/");
            let (gltf, png) = memory_card_icon(&icon.data, &relative_uri(&path, &image))?;
            let image_path = published.join("port_data").join(&image);
            if image_path.exists() && fs::read(&image_path)? != png {
                return Err(fail(format!("memory card icon image collides: {image}")));
            }
            save(&published.join("port_data").join(&path), &gltf)?;
            fs::write(&image_path, &png)?;
            report.referenced_pngs.insert(image.clone());
            report.files.push(image);
            report.files.push(path);
            converted.insert(icon.alias.clone());
        }
        if matches!(extension(&alias), "mds" | "mdt") {
            for ext in ["mot", "wgt", "bbp"] {
                let sibling = p.with_extension(ext);
                if sibling.is_file() {
                    let sibling_alias = sibling
                        .strip_prefix(raw)
                        .unwrap()
                        .to_string_lossy()
                        .replace('\\', "/");
                    discover(&fs::read(&sibling)?, &sibling_alias, &mut resources, 0)?;
                }
            }
        }
        let links = script_links(&resources);
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
            let bind_world =
                if let Some(pose) = selected_pose.filter(|_| !weight_channels.is_empty()) {
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
                    &all_textures,
                    &mut used_textures,
                    &weight_channels,
                    &bind_world,
                )?)
            }
            let mut motion_channels = Vec::new();
            let mut animation_aliases = Vec::new();
            for (alias, cs) in &parsed {
                if extension(alias) == "mot"
                    && compatible_motion(cs, frames.len())
                    && (stem(alias) == stem(&r.alias)
                        || links.get(&r.alias).is_some_and(|l| l.contains(alias)))
                {
                    motion_channels.extend(cs.clone());
                }
            }
            prepare_morphs(&mut gltf, &meshes, &motion_channels)?;
            prepare_transform_nodes(&mut gltf, &motion_channels)?;
            for (&frame, &helper) in &gltf.rotation_nodes {
                frames[frame]["rotation_node"] = json!(helper);
            }
            for (alias, cs) in &parsed {
                if extension(alias) == "mot"
                    && compatible_motion(cs, frames.len())
                    && (stem(alias) == stem(&r.alias)
                        || links.get(&r.alias).is_some_and(|l| l.contains(alias)))
                {
                    let id = animation(&mut gltf, alias, cs)?;
                    animation_aliases.push(json!({"alias":alias,"name":alias,"index":id}));
                }
            }
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
            artifacts.insert(
                r.alias.clone(),
                NativeArtifact {
                    gltf: gltf.finish(),
                    metadata,
                    animations: animation_aliases.clone(),
                },
            );
            converted.insert(r.alias.clone());
            for a in animation_aliases.iter() {
                let alias = a["alias"].as_str().unwrap();
                catalog_resources.insert(
                alias.into(),
                json!({"alias":alias,"kind":"motion","gltf":gltf_path,"metadata":metadata_path}),
            );
            }
            for pose in &bind_poses {
                let alias = pose["alias"].as_str().unwrap();
                catalog_resources.insert(alias.into(),json!({"alias":alias,"kind":"bind_pose","gltf":gltf_path,"metadata":metadata_path}));
            }
            if parsed.contains_key(&weights_alias) {
                catalog_resources.insert(weights_alias.clone(),json!({"alias":weights_alias,"kind":"weights","gltf":gltf_path,"metadata":metadata_path}));
            }
        }
        for r in &resources {
            if converted.contains(&r.alias) {
                continue;
            }
            let ext = extension(&r.alias);
            if matches!(ext, "cfg" | "clo" | "chr" | "chr_" | "img" | "tm2" | "stb") {
                continue;
            }
            if !matches!(ext, "mot" | "wgt" | "bbp") {
                return Err(fail(format!("invalid model resource {}", r.alias)));
            }
            if !catalog_resources.contains_key(&r.alias) {
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
                if ext == "mot" {
                    prepare_transform_nodes(&mut gltf, &cs)?;
                    animation(&mut gltf, &r.alias, &cs)?;
                }
                let gp = output_path(&r.alias, "gltf");
                let mp = output_path(&r.alias, "metadata.json");
                let mut metadata = json!({"schema_version":1,"source":r.alias,"frames":[],"meshes":[],"motion_channels":if ext=="mot"{cs.clone()}else{Vec::new()},"weights":if ext=="wgt"{vec![json!({"alias":r.alias,"channels":cs})]}else{Vec::new()},"bind_poses":poses.get(&r.alias).cloned().into_iter().collect::<Vec<_>>(),"frames_per_second":30});
                if !gltf.rotation_nodes.is_empty() {
                    metadata["rotation_nodes"] = rotation_map(&gltf);
                }
                artifacts.insert(
                    r.alias.clone(),
                    NativeArtifact {
                        gltf: gltf.finish(),
                        metadata,
                        animations: if ext == "mot" {
                            vec![json!({"alias":r.alias,"index":0})]
                        } else {
                            Vec::new()
                        },
                    },
                );
                catalog_resources.insert(r.alias.clone(),json!({"alias":r.alias,"kind":if ext=="mot"{"motion"}else if ext=="wgt"{"weights"}else{"bind_pose"},"gltf":gp,"metadata":mp}));
            }
            converted.insert(r.alias.clone());
        }
        publish_native(
            published,
            &resources,
            &artifacts,
            documents,
            &mut converted,
            &mut report,
        )?;
    }
    for p in &files {
        let alias = p
            .strip_prefix(raw)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        let dest = published.join("data").join(p.strip_prefix(raw).unwrap());
        if converted.contains(&alias) {
            if dest.is_file() {
                fs::remove_file(dest)?
            }
            continue;
        }
        if !dest.is_file() || fs::metadata(&dest)?.len() > 128 << 20 {
            continue;
        }
        let mut b = fs::read(&dest)?;
        if scrub(&mut b, &alias, &converted, 0)? {
            fs::write(dest, b)?
        }
    }
    Ok(report)
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
    for (index, chunk) in encoded.as_bytes().chunks_exact(4).enumerate() {
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
/// Resolves a relative glTF URI against its port_data document into a port_data path.
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
/// Checks that every glTF image references an existing standalone PNG in port_data.
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
fn reference(root: &Path, value: &Value, key: &str) -> CoreResult<PathBuf> {
    let s = value[key]
        .as_str()
        .ok_or_else(|| fail(format!("missing model {key}")))?;
    if s.is_empty()
        || s.contains(['\\', ':', '\0'])
        || Path::new(s)
            .components()
            .any(|c| !matches!(c, std::path::Component::Normal(_)))
    {
        return Err(fail("unsafe model reference"));
    }
    let path = root.join(s);
    if !path.is_file() {
        return Err(fail(format!("missing model asset {s}")));
    }
    Ok(path)
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
fn validate_rotation_nodes(value: &Value, node_count: usize) -> CoreResult<()> {
    for (node, frame) in value
        .as_object()
        .ok_or_else(|| fail("rotation node mapping is not an object"))?
    {
        let node = node
            .parse::<usize>()
            .map_err(|_| fail("invalid rotation helper index"))?;
        if node >= node_count
            || frame
                .as_u64()
                .is_none_or(|frame| frame >= node_count as u64)
        {
            return Err(fail("rotation node mapping outside skeleton"));
        }
    }
    Ok(())
}

fn validate_gltf(doc: &Value, metadata: &Value) -> CoreResult<()> {
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
    let mut buffers = Vec::new();
    if let Some(bs) = doc["buffers"].as_array() {
        for b in bs {
            let data = decode_uri(
                b["uri"]
                    .as_str()
                    .ok_or_else(|| fail("glTF buffer URI missing"))?,
                "data:application/octet-stream;base64,",
            )?;
            if b["byteLength"].as_u64() != Some(data.len() as u64) {
                return Err(fail("glTF buffer length mismatch"));
            }
            buffers.push(data);
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
    if let Some(map) = metadata.get("rotation_nodes") {
        validate_rotation_nodes(map, nodes.len())?;
    }
    if let Some(map) = metadata.get("animation_node_offsets") {
        for (animation, offset) in map
            .as_object()
            .ok_or_else(|| fail("animation node offsets is not an object"))?
        {
            let animation = animation
                .parse::<usize>()
                .map_err(|_| fail("invalid animation offset index"))?;
            if animation >= doc["animations"].as_array().map_or(0, Vec::len)
                || offset
                    .as_u64()
                    .is_none_or(|offset| offset == 0 || offset >= nodes.len() as u64)
            {
                return Err(fail("animation node offset outside asset"));
            }
            let offset = offset.as_u64().unwrap();
            for channel in doc["animations"][animation]["channels"].as_array().unwrap() {
                if channel["target"]["node"]
                    .as_u64()
                    .is_none_or(|node| node < offset)
                {
                    return Err(fail("animation target precedes its source skeleton"));
                }
            }
        }
    }
    Ok(())
}

fn validate_animation_variant(doc: &Value, metadata: &Value, variant: &Value) -> CoreResult<()> {
    let nodes = doc["nodes"]
        .as_array()
        .ok_or_else(|| fail("animation nodes missing"))?;
    let animation = index(
        &variant["animation"],
        doc["animations"].as_array().map_or(0, Vec::len),
        "animation variant",
    )?;
    let offset = index(
        &variant["node_offset"],
        nodes.len(),
        "animation skeleton offset",
    )?;
    let mut helpers = BTreeMap::new();
    let frame_count = if variant["model"].is_null() {
        if let Some(map) = variant.get("rotation_nodes") {
            validate_rotation_nodes(map, nodes.len())?;
            for (node, frame) in map.as_object().unwrap() {
                helpers.insert(
                    node.parse::<usize>().unwrap(),
                    frame.as_u64().unwrap() as usize,
                );
            }
        }
        None
    } else {
        let model = index(
            &variant["model"],
            metadata["models"].as_array().map_or(0, Vec::len),
            "animation model",
        )?;
        let model = &metadata["models"][model];
        if model["node_offset"].as_u64() != Some(offset as u64) {
            return Err(fail("animation/model skeleton offset mismatch"));
        }
        let frames = model["frames"]
            .as_array()
            .ok_or_else(|| fail("animation model frames missing"))?;
        for (frame, value) in frames.iter().enumerate() {
            if let Some(node) = value["rotation_node"].as_u64() {
                helpers.insert(node as usize, frame);
            }
        }
        Some(frames.len())
    };
    for (&node, &frame) in &helpers {
        let original = offset + frame;
        if original >= nodes.len()
            || !nodes[original]["children"]
                .as_array()
                .is_some_and(|children| {
                    children
                        .iter()
                        .any(|child| child.as_u64() == Some(node as u64))
                })
        {
            return Err(fail("animation rotation helper/source frame mismatch"));
        }
    }
    for channel in doc["animations"][animation]["channels"].as_array().unwrap() {
        let node = channel["target"]["node"].as_u64().unwrap() as usize;
        let frame = helpers
            .get(&node)
            .copied()
            .or_else(|| node.checked_sub(offset))
            .ok_or_else(|| fail("animation target before skeleton"))?;
        if frame_count.is_some_and(|count| frame >= count) {
            return Err(fail("animation target outside native frames"));
        }
    }
    Ok(())
}

/// Verifies direct glTF assets, embedded semantic resources and remaining retail payloads.
pub fn verify(root: &Path) -> CoreResult<()> {
    let port = root.join("port_data");
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
    let mut checked_pngs = BTreeSet::new();
    for path in files {
        if path.extension().and_then(|s| s.to_str()) != Some("gltf") {
            continue;
        }
        let doc: Value = serde_json::from_slice(&fs::read(&path)?)?;
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
        validate_gltf(&doc, &metadata)?;
        let document = path
            .strip_prefix(&port)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        verify_images(&port, &document, &doc, &metadata, &names, &mut checked_pngs)?;
        if metadata["kind"] == "character" {
            for model in metadata["models"]
                .as_array()
                .ok_or_else(|| fail("character models missing"))?
            {
                validate_gltf(&doc, model)?;
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
                    reference(&port, resource, "path")?;
                }
            }
            for animation in resources["animations"]
                .as_array()
                .ok_or_else(|| fail("character animations missing"))?
            {
                for variant in animation["variants"].as_array().into_iter().flatten() {
                    validate_animation_variant(&doc, &metadata, variant)?;
                }
                if !animation["animation"].is_null() {
                    index(
                        &animation["animation"],
                        doc["animations"].as_array().map_or(0, Vec::len),
                        "character animation",
                    )?;
                }
            }
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
                                levels.extend(variant["levels"].as_array().ok_or_else(|| {
                                    fail("embedded texture variant levels missing")
                                })?);
                            }
                        }
                        for level in levels {
                            let id = level
                                .as_str()
                                .and_then(|s| s.strip_prefix("#/images/"))
                                .and_then(|s| s.parse::<usize>().ok())
                                .ok_or_else(|| fail("invalid embedded texture image URI"))?;
                            if id >= doc["images"].as_array().map_or(0, Vec::len) {
                                return Err(fail("embedded texture image out of range"));
                            }
                        }
                    }
                }
            }
        }
    }
    let data = root.join("data");
    if data.is_dir() {
        let mut files = Vec::new();
        collect(&data, &mut files)?;
        for file in files {
            if fs::metadata(&file)?.len() > 128 << 20 {
                continue;
            }
            let alias = file
                .strip_prefix(&data)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
            let mut resources = Vec::new();
            discover(&fs::read(&file)?, &alias, &mut resources, 0)?;
            if let Some(resource) = resources.iter().find(|r| {
                r.data.starts_with(b"MDS ")
                    || r.data.starts_with(b"MDT ")
                    || matches!(
                        extension(&r.alias),
                        "chr" | "chr_" | "mds" | "mdt" | "mot" | "wgt" | "bbp" | "ico"
                    )
            }) {
                return Err(fail(format!(
                    "proprietary model payload remains: {}",
                    resource.alias
                )));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
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
        convert_tree(&temp.path().join("raw"), &temp.path().join("out"), 1, &[]).unwrap();
        verify(&temp.path().join("out")).unwrap();
        assert!(temp.path().join("out/port_data/title/icon.gltf").is_file());
        let image = temp.path().join("out/port_data/title/icon.png");
        assert_eq!(fs::read(&image).unwrap(), png);
        let published: Value = serde_json::from_slice(
            &fs::read(temp.path().join("out/port_data/title/icon.gltf")).unwrap(),
        )
        .unwrap();
        assert_eq!(published["images"], json!([{"uri":"icon.png"}]));
        fs::remove_file(&image).unwrap();
        assert!(verify(&temp.path().join("out")).is_err());
        fs::write(&image, &png).unwrap();
        assert!(
            !temp
                .path()
                .join("out/port_data/title/icon.game.json")
                .exists()
        );
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
        let port = temp.path().join("port_data");
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
        fs::create_dir_all(temp.path().join("port_data")).unwrap();
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
        fs::write(temp.path().join("port_data/plain.png"), png).unwrap();
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
        let view = gltf.doc["accessors"][accessor]["bufferView"]
            .as_u64()
            .unwrap() as usize;
        let view = &gltf.doc["bufferViews"][view];
        floats(
            &gltf.buffer,
            view["byteOffset"].as_u64().unwrap() as usize,
            view["byteLength"].as_u64().unwrap() as usize / 4,
        )
        .unwrap()
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
        prepare_transform_nodes(&mut gltf, &channels).unwrap();
        assert_eq!(gltf.rotation_nodes[&0], 2);
        assert!(gltf.doc["nodes"][0].get("mesh").is_none());
        assert_eq!(gltf.doc["nodes"][2]["mesh"], 0);
        assert_eq!(gltf.doc["nodes"][2]["children"], json!([1]));
        assert_eq!(gltf.doc["nodes"][2]["weights"], json!([0, 0, 0]));
        assert_eq!(gltf.doc["skins"][0]["joints"], json!([2]));
        animation(&mut gltf, "actual-p00-frame74-time14", &channels).unwrap();
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
            prepare_transform_nodes(&mut gltf, &channels).unwrap();
            assert_eq!(gltf.doc["nodes"][1]["matrix"], json!(matrix));
            animation(&mut gltf, "reset", &channels).unwrap();
            let animation = &gltf.doc["animations"][0];
            let reset = animation["channels"]
                .as_array()
                .unwrap()
                .iter()
                .find(|channel| channel["target"]["path"] == "scale")
                .unwrap();
            assert_eq!(reset["target"]["node"], 1);
            let sampler = reset["sampler"].as_u64().unwrap() as usize;
            assert_eq!(
                test_accessor(
                    &gltf,
                    animation["samplers"][sampler]["output"].as_u64().unwrap() as usize
                ),
                vec![1., 1., 1.]
            );
            assert!(
                (gltf.doc["nodes"][1]["scale"][0].as_f64().unwrap() - scale[0] as f64).abs() < 1e-6
            );
        }
    }
    #[test]
    fn merged_character_and_animation_only_helpers_keep_global_standard_indices() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        fs::create_dir_all(&raw).unwrap();
        let mut model = vec![0; 128];
        model[..4].copy_from_slice(b"MDS ");
        put(&mut model, 8, 1);
        put(&mut model, 12, 16);
        put(&mut model, 20, 112);
        put(&mut model, 60, u32::MAX);
        for (i, value) in identity().iter().enumerate() {
            put(&mut model, 64 + i * 4, value.to_bits());
        }
        let mut motion = vec![0; 128];
        for (at, kind, more, values) in
            [(0, 0, 1, [1f32, 0., 0., 0.]), (64, 1, 0, [2., 1., 1., 0.])]
        {
            put(&mut motion, at + 8, kind);
            put(&mut motion, at + 16, 1);
            put(&mut motion, at + 20, more);
            for (i, value) in values.iter().enumerate() {
                put(&mut motion, at + 48 + i * 4, value.to_bits());
            }
        }
        let mut standalone = motion.clone();
        put(&mut standalone, 0, 2);
        put(&mut standalone, 64, 2);
        let character = test_pack(&[
            ("a.mds", &model),
            ("a.mot", &motion),
            ("b.mds", &model),
            ("b.mot", &motion),
            ("orphan.mot", &standalone),
            ("info.cfg", b"MODEL \"a.mds\";MOTION 0,\"a.mot\",\"\",\"\";"),
        ]);
        fs::write(raw.join("hero.chr"), character).unwrap();
        let published = temp.path().join("published");
        let textures = super::super::textures::convert_tree(&raw, &published, 1).unwrap();
        convert_tree(&raw, &published, 1, &textures.documents).unwrap();
        verify(&published).unwrap();
        let game: Value =
            serde_json::from_slice(&fs::read(published.join("port_data/hero.game.json")).unwrap())
                .unwrap();
        assert_eq!(game["models"][0]["frames"][0]["rotation_node"], 1);
        assert_eq!(game["models"][1]["frames"][0]["rotation_node"], 3);
        let orphan = game["resources"]["animations"]
            .as_array()
            .unwrap()
            .iter()
            .find(|animation| animation["variants"][0]["model"].is_null())
            .unwrap();
        assert_eq!(orphan["variants"][0]["node_offset"], 4);
        assert_eq!(orphan["variants"][0]["rotation_nodes"], json!({"7":2}));
        assert_eq!(shifted_rotation_map(&json!({"3":2}), 4), json!({"7":2}));
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
            animation(
                &mut gltf,
                "rotation",
                &[json!({"frame":0,"type":0,"keys":[{"frame":0,"value":raw}]})],
            )
            .unwrap();
            let output = gltf.doc["animations"][0]["samplers"][0]["output"]
                .as_u64()
                .unwrap() as usize;
            let view = gltf.doc["accessors"][output]["bufferView"]
                .as_u64()
                .unwrap() as usize;
            let offset = gltf.doc["bufferViews"][view]["byteOffset"]
                .as_u64()
                .unwrap() as usize;
            let q = floats(&gltf.buffer, offset, 4).unwrap();
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
        model[..4].copy_from_slice(b"MDS ");
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
        convert_tree(&raw, &published, 1, &textures.documents).unwrap();
        let doc: Value = serde_json::from_slice(
            &fs::read(published.join("port_data/gsys.pak/hake.game.json")).unwrap(),
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
        let error = convert_tree(&raw, &other, 1, &textures.documents)
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
            "hero.chr",
            &refs,
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
        animation(&mut g, "vertex.mot", &cs).unwrap();
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
    fn incompatible_motion_keeps_every_standard_track_in_standalone_gltf() {
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
        assert_eq!(animation(&mut g, "e213a.mot", &cs).unwrap(), Some(0));
        let doc = g.finish();
        assert_eq!(doc["animations"][0]["channels"][0]["target"]["node"], 65);
        validate_gltf(&doc, &json!({"schema_version":1,"frames":[]})).unwrap();
    }
    #[test]
    fn same_stem_incompatible_standalone_clip_does_not_overwrite_model() {
        let temp = tempfile::tempdir().unwrap();
        let mut geometry = Gltf::new();
        geometry.doc["nodes"] = json!([{"name":"model_root","matrix":identity()}]);
        geometry.doc["scenes"][0]["nodes"] = json!([0]);
        let cs =
            vec![json!({"frame":2,"target":0,"type":2,"keys":[{"frame":0,"value":[0,0,0,1]}]})];
        let mut motion = Gltf::new();
        motion.doc["nodes"] = json!([{"name":"frame_0"},{"name":"frame_1"},{"name":"frame_2"}]);
        animation(&mut motion, "hero.mot", &cs).unwrap();
        let base = json!({"schema_version":1,"frames":[],"meshes":[],"motion_channels":[],"weights":[],"bind_poses":[]});
        let mut metadata = base.clone();
        metadata["frames"] = json!([{"id":0,"mdt":null}]);
        let mut motion_meta = base;
        motion_meta["motion_channels"] = json!(cs);
        let artifacts = BTreeMap::from([
            (
                "hero.mds".into(),
                NativeArtifact {
                    gltf: geometry.finish(),
                    metadata,
                    animations: vec![],
                },
            ),
            (
                "hero.mot".into(),
                NativeArtifact {
                    gltf: motion.finish(),
                    metadata: motion_meta,
                    animations: vec![],
                },
            ),
        ]);
        let mut report = ConversionReport::default();
        publish_native(
            temp.path(),
            &[],
            &artifacts,
            &[],
            &mut BTreeSet::new(),
            &mut report,
        )
        .unwrap();
        let doc: Value =
            serde_json::from_slice(&fs::read(temp.path().join("port_data/hero.gltf")).unwrap())
                .unwrap();
        assert_eq!(doc["nodes"][0]["name"], "model_root");
        assert_eq!(doc["animations"][0]["channels"][0]["target"]["node"], 3);
        assert_eq!(doc["nodes"].as_array().unwrap().len(), 4);
        let game: Value = serde_json::from_slice(
            &fs::read(temp.path().join("port_data/hero.game.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(game["animation_node_offsets"], json!({"0":1}));
        validate_gltf(&doc, &game).unwrap();
        let mut broken = game.clone();
        broken["animation_node_offsets"]["0"] = json!(4);
        assert!(validate_gltf(&doc, &broken).is_err());
        verify(temp.path()).unwrap();
    }

    #[test]
    fn shifted_standalone_clip_keeps_global_rotation_helper_and_local_frame() {
        let temp = tempfile::tempdir().unwrap();
        let mut geometry = Gltf::new();
        geometry.doc["nodes"] = json!([{"matrix":identity()}]);
        geometry.doc["scenes"][0]["nodes"] = json!([0]);
        let channels = vec![
            json!({"frame":2,"target":0,"type":0,"keys":[{"frame":0,"value":[1,0,0,0]}]}),
            json!({"frame":2,"target":0,"type":1,"keys":[{"frame":0,"value":[0.9,1,1,1]}]}),
        ];
        let mut motion = Gltf::new();
        motion.doc["nodes"] = json!([{}, {}, {}]);
        prepare_transform_nodes(&mut motion, &channels).unwrap();
        animation(&mut motion, "hero.mot", &channels).unwrap();
        let rotation_nodes = rotation_map(&motion);
        let artifacts = BTreeMap::from([
            (
                "hero.mds".into(),
                NativeArtifact {
                    gltf: geometry.finish(),
                    metadata: json!({"schema_version":1,"frames":[{"mdt":null}],"meshes":[],"motion_channels":[],"weights":[],"bind_poses":[]}),
                    animations: vec![],
                },
            ),
            (
                "hero.mot".into(),
                NativeArtifact {
                    gltf: motion.finish(),
                    metadata: json!({"schema_version":1,"frames":[],"meshes":[],"motion_channels":channels,"weights":[],"bind_poses":[],"rotation_nodes":rotation_nodes}),
                    animations: vec![],
                },
            ),
        ]);
        publish_native(
            temp.path(),
            &[],
            &artifacts,
            &[],
            &mut BTreeSet::new(),
            &mut ConversionReport::default(),
        )
        .unwrap();
        let doc: Value =
            serde_json::from_slice(&fs::read(temp.path().join("port_data/hero.gltf")).unwrap())
                .unwrap();
        let game: Value = serde_json::from_slice(
            &fs::read(temp.path().join("port_data/hero.game.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(game["animation_node_offsets"], json!({"0":1}));
        assert_eq!(game["rotation_nodes"], json!({"4":2}));
        assert_eq!(doc["nodes"][3]["children"], json!([4]));
        assert_eq!(doc["animations"][0]["channels"][0]["target"]["node"], 4);
        assert_eq!(doc["animations"][0]["channels"][1]["target"]["node"], 3);
        verify(temp.path()).unwrap();
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
        convert_tree(&raw, &output, 1, &[]).unwrap();
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
        convert_tree(&raw, &published, 1, &[]).unwrap();
        verify(&published).unwrap();
        let doc: Value = serde_json::from_slice(
            &fs::read(published.join("port_data/fixture.game.json")).unwrap(),
        )
        .unwrap();
        assert!(!doc["models"].as_array().unwrap().is_empty());
        assert!(!published.join("data/fixture.chr").exists());
        if let Some(path) = std::env::var_os("KZ_MODEL_FIXTURE_OUTPUT") {
            fs::copy(published.join("port_data/fixture.gltf"), &path).unwrap();
            fs::copy(
                published.join("port_data/fixture.game.json"),
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
        let report = convert_tree(&raw, &output, 4, &[]).unwrap();
        let mut counts = json!({"assets":0,"models":0,"animations":0,"variants":0,"null_variants":0,"shifted_standalone_clips":0,"rotation_helpers":0});
        for file in report.files.iter().filter(|file| file.ends_with(".gltf")) {
            let path = output.join("port_data").join(file);
            let doc: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
            counts["assets"] = json!(counts["assets"].as_u64().unwrap() + 1);
            if !path.with_extension("game.json").is_file() {
                continue;
            }
            let metadata: Value =
                serde_json::from_slice(&fs::read(path.with_extension("game.json")).unwrap())
                    .unwrap();
            validate_gltf(&doc, &metadata).unwrap_or_else(|error| panic!("{file}: {error}"));
            counts["animations"] = json!(
                counts["animations"].as_u64().unwrap()
                    + doc["animations"].as_array().map_or(0, Vec::len) as u64
            );
            counts["shifted_standalone_clips"] = json!(
                counts["shifted_standalone_clips"].as_u64().unwrap()
                    + metadata["animation_node_offsets"]
                        .as_object()
                        .map_or(0, serde_json::Map::len) as u64
            );
            let models: Vec<_> = if metadata["kind"] == "character" {
                metadata["models"].as_array().unwrap().iter().collect()
            } else {
                vec![&metadata]
            };
            for model in models {
                validate_gltf(&doc, model).unwrap_or_else(|error| panic!("{file}: {error}"));
                counts["models"] = json!(
                    counts["models"].as_u64().unwrap()
                        + u64::from(
                            model["frames"]
                                .as_array()
                                .is_some_and(|frames| !frames.is_empty())
                        )
                );
                counts["rotation_helpers"] = json!(
                    counts["rotation_helpers"].as_u64().unwrap()
                        + model["frames"]
                            .as_array()
                            .into_iter()
                            .flatten()
                            .filter(|frame| frame.get("rotation_node").is_some())
                            .count() as u64
                );
            }
            for resource in metadata["resources"]["animations"]
                .as_array()
                .into_iter()
                .flatten()
            {
                for variant in resource["variants"].as_array().into_iter().flatten() {
                    counts["variants"] = json!(counts["variants"].as_u64().unwrap() + 1);
                    counts["null_variants"] = json!(
                        counts["null_variants"].as_u64().unwrap()
                            + u64::from(variant["model"].is_null())
                    );
                    validate_animation_variant(&doc, &metadata, variant)
                        .unwrap_or_else(|error| panic!("{file}: {error}"));
                }
            }
        }
        save(&output.join("transform-audit.json"), &counts).unwrap();
        println!("{}", serde_json::to_string_pretty(&counts).unwrap());
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
        convert_tree(&raw, &output, 4, &converted.documents).unwrap();
        verify(&output).unwrap();
    }
}
