//! Converts retail sound resources to PCM WAV and semantic JSON before publication.
use super::archive::ResourceRef;
use super::artifacts::{ArtifactBytes, AssetSink, Category, Job};
use super::inventory::Inventory;
use super::progress::Phase;
use super::{CoreError, CoreResult};
use rayon::prelude::*;
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Condvar, Mutex, PoisonError};

fn fail(message: impl Into<String>) -> CoreError {
    CoreError(message.into())
}
fn u16le(b: &[u8], at: usize) -> CoreResult<u16> {
    Ok(u16::from_le_bytes(
        b.get(at..at + 2)
            .ok_or_else(|| fail("truncated audio u16"))?
            .try_into()
            .unwrap(),
    ))
}
fn u32le(b: &[u8], at: usize) -> CoreResult<u32> {
    Ok(u32::from_le_bytes(
        b.get(at..at + 4)
            .ok_or_else(|| fail("truncated audio u32"))?
            .try_into()
            .unwrap(),
    ))
}
fn tag(b: &[u8], at: usize, name: &[u8]) -> bool {
    b.get(at..at + 4)
        .is_some_and(|s| s == name || s.iter().rev().copied().eq(name.iter().copied()))
}
fn chunk<'a>(b: &'a [u8], name: &[u8]) -> CoreResult<&'a [u8]> {
    let at = (0..b.len().saturating_sub(7))
        .step_by(4)
        .find(|&i| tag(b, i, b"SCEI") && tag(b, i + 4, name))
        .ok_or_else(|| {
            fail(format!(
                "audio chunk {} missing",
                String::from_utf8_lossy(name)
            ))
        })?;
    let size = u32le(b, at + 8)? as usize;
    b.get(at..at + size)
        .filter(|s| s.len() >= 16)
        .ok_or_else(|| fail("invalid audio chunk size"))
}
fn entries(b: &[u8]) -> CoreResult<Vec<&[u8]>> {
    let last = u32le(b, 12)?;
    if last == u32::MAX {
        return Ok(vec![]);
    }
    let count = last as usize + 1;
    if count > (b.len().saturating_sub(16)) / 4 {
        return Err(fail("audio table exceeds chunk"));
    }
    (0..count)
        .map(|i| {
            let at = u32le(b, 16 + i * 4)?;
            if at == u32::MAX {
                return Ok(&b[..0]);
            }
            b.get(at as usize..)
                .ok_or_else(|| fail("audio table offset exceeds chunk"))
        })
        .collect()
}

/// Decodes one SPU sample, retaining its sample-frame loop boundary.
fn adpcm(b: &[u8]) -> CoreResult<(Vec<i16>, usize, bool)> {
    const FILTER: [[i32; 2]; 5] = [[0, 0], [60, 0], [115, -52], [98, -55], [122, -60]];
    let mut out = Vec::new();
    let (mut h0, mut h1, mut loop_start, mut loops) = (0, 0, 0, false);
    for block in b.as_chunks::<16>().0 {
        let shift = if block[0] & 15 > 12 { 9 } else { block[0] & 15 };
        let filter = FILTER[usize::from(block[0] >> 4).min(4)];
        if block[1] & 4 != 0 {
            loop_start = out.len();
        }
        for i in 0..28 {
            let nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 15;
            let sample = (((nibble as i16) << 12) as i32 >> shift)
                + ((h0 * filter[0] + h1 * filter[1] + 32) >> 6);
            h1 = h0;
            h0 = sample.clamp(-32768, 32767);
            out.push(h0 as i16);
        }
        if block[1] & 1 != 0 {
            loops = block[1] & 2 != 0;
            break;
        }
    }
    if out.is_empty() {
        return Err(fail("empty or truncated SPU sample"));
    }
    Ok((out, loop_start, loops))
}
fn wav(pcm: &[i16], rate: u32, channels: u16) -> CoreResult<Vec<u8>> {
    if rate == 0 || !(1..=2).contains(&channels) || !pcm.len().is_multiple_of(channels as usize) {
        return Err(fail("invalid PCM layout"));
    }
    let bytes = u32::try_from(pcm.len() * 2).map_err(|_| fail("PCM WAV exceeds RIFF size"))?;
    let mut out = Vec::with_capacity(bytes as usize + 44);
    out.extend(b"RIFF");
    out.extend((bytes + 36).to_le_bytes());
    out.extend(b"WAVEfmt ");
    out.extend(16u32.to_le_bytes());
    out.extend(1u16.to_le_bytes());
    out.extend(channels.to_le_bytes());
    out.extend(rate.to_le_bytes());
    out.extend((rate * channels as u32 * 2).to_le_bytes());
    out.extend((channels * 2).to_le_bytes());
    out.extend(16u16.to_le_bytes());
    out.extend(b"data");
    out.extend(bytes.to_le_bytes());
    for sample in pcm {
        out.extend(sample.to_le_bytes());
    }
    Ok(out)
}
/// Identifies control files owned by sound bundles rather than game scripts.
pub(crate) fn owns_config(alias: &str) -> bool {
    alias.split('/').any(|part| part.ends_with(".snd"))
        && (alias.ends_with(".cfg") || alias.ends_with(".vol"))
}
fn path(key: &str, suffix: &str) -> String {
    let encoded: String = key
        .bytes()
        .map(|b| {
            if b.is_ascii_alphanumeric() || b"._-/".contains(&b) {
                (b as char).to_string()
            } else {
                format!("%{b:02x}")
            }
        })
        .collect();
    let (base, member) = if let Some((bundle, member)) = encoded.split_once(".snd/") {
        (format!("{bundle}.sound"), member)
    } else {
        (
            format!(
                "{}.sound",
                encoded.rsplit_once('.').map_or(encoded.as_str(), |v| v.0)
            ),
            "",
        )
    };
    let stem = member.rsplit_once('.').map_or(member, |v| v.0);
    match suffix {
        "bank.json" => format!("{base}/bank.json"),
        "sequence.json" => format!(
            "{base}/{}/{stem}.json",
            if member.ends_with(".mid") {
                "effects"
            } else {
                "sequences"
            }
        ),
        "config.json" => format!("{base}/config.json"),
        "volume.json" => format!("{base}/volume.json"),
        value if value.starts_with("sample-") => {
            format!("{base}/samples/{}", value.trim_start_matches("sample-"))
        }
        value => format!("{base}/{value}"),
    }
}
fn write(root: &Path, relative: &str, bytes: &[u8]) -> CoreResult<()> {
    let target = root.join(relative);
    fs::create_dir_all(target.parent().unwrap())?;
    fs::write(target, bytes)?;
    Ok(())
}
fn write_json(root: &Path, relative: &str, value: &Value) -> CoreResult<()> {
    let mut b = serde_json::to_vec_pretty(value)?;
    b.push(b'\n');
    write(root, relative, &b)
}

fn bank_with(
    key: &str,
    hd: &[u8],
    bd: &[u8],
    emit: &mut impl FnMut(&str, Vec<u8>) -> CoreResult<()>,
) -> CoreResult<String> {
    let programs=entries(chunk(hd,b"Prog")?)?.into_iter().map(|p| {
        if p.is_empty() { return Ok(json!({"present":false})); }
        if p.len()<36 {return Err(fail("truncated bank program"));}
        let base=u32le(p,0)? as usize; let stride=usize::from(p[5]).max(20);
        let splits=(0..p[4] as usize).map(|i| {
            let s=p.get(base+i*stride..base+i*stride+20).ok_or_else(|| fail("truncated bank split"))?;
            Ok(json!({"sample_set":u16le(s,0)?,"key_low":s[2],"key_high":s[4],"bend_low":u16le(s,6)?,"bend_high":u16le(s,8)?,"volume":s[16],"pan":s[17],"transpose":s[18] as i8,"detune":s[19] as i8}))
        }).collect::<CoreResult<Vec<_>>>()?;
        Ok(json!({"present":true,"volume":p[6],"pan":p[7],"transpose":p[8] as i8,"detune":p[9] as i8,"splits":splits}))
    }).collect::<CoreResult<Vec<_>>>()?;
    let sets = match chunk(hd, b"Sset") {
        Ok(b) => entries(b)?
            .into_iter()
            .map(|p| {
                if p.is_empty() {
                    return Ok(json!({"vel_curve":0,"vel_low":0,"vel_high":0,"samples":[]}));
                }
                if p.len() < 4 {
                    return Err(fail("truncated sample set"));
                }
                let samples = (0..p[3] as usize)
                    .map(|i| u16le(p, 4 + i * 2))
                    .collect::<CoreResult<Vec<_>>>()?;
                Ok(json!({"vel_curve":p[0],"vel_low":p[1],"vel_high":p[2],"samples":samples}))
            })
            .collect::<CoreResult<Vec<_>>>()?,
        Err(_) => vec![],
    };
    let samples=entries(chunk(hd,b"Smpl")?)?.into_iter().map(|p| {
        if p.is_empty() { return Ok(json!({"vag":-1})); }
        if p.len()<42 {return Err(fail("truncated sample parameters"));}
        Ok(json!({"vag":u16le(p,0)?,"vel_low":p[2],"vel_high":p[4],"base_note":p[11],"detune":p[12] as i8,"pan":p[13],"volume":p[16],"adsr1":u16le(p,18)?,"adsr2":u16le(p,20)?,"spu_attr":p[41]}))
    }).collect::<CoreResult<Vec<_>>>()?;
    let mut vags = Vec::new();
    for (index, p) in entries(chunk(hd, b"Vagi")?)?.into_iter().enumerate() {
        if p.is_empty() {
            vags.push(Value::Null);
            continue;
        }
        let offset = u32le(p, 0)? as usize;
        let rate = u16le(p, 4)?;
        let (pcm, loop_start, loops) =
            adpcm(bd.get(offset..).ok_or_else(|| fail("sample outside BD"))?)?;
        let sample_path = path(key, &format!("sample-{index}.wav"));
        emit(&sample_path, wav(&pcm, rate as u32, 1)?)?;
        vags.push(json!({"wav":sample_path,"attr":p[6],"loop_start":loop_start,"loop_end":pcm.len(),"loops":loops}));
    }
    let document = path(key, "bank.json");
    emit(
        &document,
        serde_json::to_vec(
            &json!({"schema_version":1,"kind":"bank","programs":programs,"sample_sets":sets,"samples":samples,"vags":vags}),
        )?,
    )?;
    Ok(document)
}

fn bank(root: &Path, key: &str, hd: &[u8], bd: &[u8]) -> CoreResult<String> {
    bank_with(key, hd, bd, &mut |name, bytes| write(root, name, &bytes))
}

fn varlen(b: &[u8], at: &mut usize) -> CoreResult<u32> {
    let mut n = 0;
    for _ in 0..4 {
        let v = *b
            .get(*at)
            .ok_or_else(|| fail("truncated sequence varlen"))?;
        *at += 1;
        n = (n << 7) | (v & 127) as u32;
        if v & 128 == 0 {
            return Ok(n);
        }
    }
    Ok(n)
}
fn events(b: &[u8]) -> CoreResult<Vec<Value>> {
    let (mut at, mut running, mut no_delta) = (0, 0, false);
    let mut out = Vec::new();
    while at < b.len() {
        let delta = if no_delta {
            no_delta = false;
            0
        } else {
            varlen(b, &mut at)?
        };
        let mut status = *b.get(at).ok_or_else(|| fail("truncated sequence status"))?;
        if status & 128 != 0 {
            at += 1;
        } else if running != 0 {
            status = running;
        } else {
            at += 1;
            continue;
        }
        if status == 255 || status == 240 || status == 247 {
            let meta = if status == 255 {
                let m = *b.get(at).ok_or_else(|| fail("truncated meta type"))?;
                at += 1;
                m
            } else {
                0
            };
            let n = varlen(b, &mut at)? as usize;
            let payload = b
                .get(at..at + n)
                .ok_or_else(|| fail("truncated sequence payload"))?;
            at += n;
            let kind = if meta == 47 {
                "end"
            } else if status == 255 {
                "meta"
            } else {
                "sysex"
            };
            out.push(json!({"delta":delta,"kind":kind,"meta":meta,"payload":payload}));
            if kind == "end" {
                break;
            }
            continue;
        }
        if status >= 240 {
            let n = if status == 242 {
                2
            } else if status == 241 || status == 243 {
                1
            } else {
                0
            };
            if at + n > b.len() {
                return Err(fail("truncated system sequence event"));
            }
            at += n;
            out.push(json!({"delta":delta,"kind":"sysex","payload":[]}));
            continue;
        }
        running = status;
        let n = if matches!(status & 240, 128 | 192 | 208) {
            1
        } else {
            2
        };
        let data = b
            .get(at..at + n)
            .ok_or_else(|| fail("truncated channel event"))?;
        no_delta = data[n - 1] & 128 != 0;
        at += n;
        out.push(json!({"delta":delta,"kind":"channel","status":status,"data":[data[0]&127,if n==2 {data[1]&127} else {0}]}));
    }
    Ok(out)
}
fn sequence_with(
    key: &str,
    sq: &[u8],
    emit: &mut impl FnMut(&str, Vec<u8>) -> CoreResult<()>,
) -> CoreResult<String> {
    let midi = chunk(sq, b"Midi")?;
    let blocks = entries(midi)?;
    let mut songs = Vec::new();
    for p in blocks {
        if p.is_empty() {
            songs.push(json!({"division":480,"events":[]}));
            continue;
        }
        let start = midi.len() - p.len();
        let mut end = midi.len();
        for i in 0..=u32le(midi, 12)? as usize {
            let n = u32le(midi, 16 + i * 4)? as usize;
            if n > start {
                end = end.min(n);
            }
        }
        let p = &midi[start..end];
        let offset = (u32le(p, 0)? as usize).max(6);
        let division = u16le(p, 4)?;
        let division = if division == 0 || division >= 32768 {
            480
        } else {
            division
        };
        songs.push(json!({"division":division,"events":events(p.get(offset..).ok_or_else(||fail("SQ data offset outside song"))?)?}));
    }
    let document = path(key, "sequence.json");
    emit(
        &document,
        serde_json::to_vec(&json!({"schema_version":1,"kind":"sequence","songs":songs}))?,
    )?;
    Ok(document)
}

fn sequence(root: &Path, key: &str, sq: &[u8]) -> CoreResult<String> {
    sequence_with(key, sq, &mut |name, bytes| write(root, name, &bytes))
}

fn smf_with(
    key: &str,
    b: &[u8],
    emit: &mut impl FnMut(&str, Vec<u8>) -> CoreResult<()>,
) -> CoreResult<String> {
    fn be16(b: &[u8], at: usize) -> CoreResult<u16> {
        Ok(u16::from_be_bytes(
            b.get(at..at + 2)
                .ok_or_else(|| fail("truncated MIDI field"))?
                .try_into()
                .unwrap(),
        ))
    }
    fn be32(b: &[u8], at: usize) -> CoreResult<u32> {
        Ok(u32::from_be_bytes(
            b.get(at..at + 4)
                .ok_or_else(|| fail("truncated MIDI field"))?
                .try_into()
                .unwrap(),
        ))
    }
    if b.get(..4) != Some(b"MThd") || be16(b, 8)? != 0 || be16(b, 10)? != 1 {
        return Err(fail(
            "unsupported effect MIDI format (expected format 0, one track)",
        ));
    }
    let division = be16(b, 12)?;
    if division == 0 || division & 32768 != 0 {
        return Err(fail("unsupported MIDI time division"));
    }
    let track = 8 + be32(b, 4)? as usize;
    if b.get(track..track + 4) != Some(b"MTrk") {
        return Err(fail("MIDI track missing"));
    }
    let b = b
        .get(track + 8..track + 8 + be32(b, track + 4)? as usize)
        .ok_or_else(|| fail("truncated MIDI track"))?;
    let (mut at, mut running) = (0, 0);
    let mut out = Vec::new();
    while at < b.len() {
        let delta = varlen(b, &mut at)?;
        let mut status = *b.get(at).ok_or_else(|| fail("truncated MIDI event"))?;
        if status & 128 != 0 {
            at += 1;
        } else if running != 0 {
            status = running;
        } else {
            return Err(fail("MIDI running status absent"));
        }
        if status == 255 {
            let meta = *b.get(at).ok_or_else(|| fail("truncated MIDI meta"))?;
            at += 1;
            let size = varlen(b, &mut at)? as usize;
            if at + size > b.len() {
                return Err(fail("truncated MIDI meta payload"));
            }
            at += size;
            if meta == 47 {
                break;
            }
            // The retail effect sequencer discards meta events and their deltas.
            continue;
        }
        if status >= 240 {
            return Err(fail("unsupported effect MIDI system event"));
        }
        running = status;
        let n = if matches!(status & 240, 192 | 208) {
            1
        } else {
            2
        };
        let data = b
            .get(at..at + n)
            .ok_or_else(|| fail("truncated MIDI channel event"))?;
        at += n;
        out.push(
            json!({"delta":delta as i16,"status":status,"data":[data[0],if n==2 {data[1]}else{0}]}),
        );
    }
    let document = path(key, "sequence.json");
    emit(
        &document,
        serde_json::to_vec(
            &json!({"schema_version":1,"kind":"effect_sequence","tick_rate":division as u32*225/60,"events":out}),
        )?,
    )?;
    Ok(document)
}

fn smf(root: &Path, key: &str, b: &[u8]) -> CoreResult<String> {
    smf_with(key, b, &mut |name, bytes| write(root, name, &bytes))
}

fn sound_rows(b: &[u8], count: usize) -> Vec<Vec<Vec<u8>>> {
    sound_rows_separated(b, count, false)
}
fn sound_rows_separated(b: &[u8], count: usize, tabs_only: bool) -> Vec<Vec<Vec<u8>>> {
    let mut columns = vec![Vec::new(); count];
    let mut rows = Vec::new();
    let mut at = 0;
    while at < b.len() {
        let mut column = 0;
        while at < b.len() {
            if matches!(b[at], b'\r' | b'\n') {
                let first = b[at];
                at += 1;
                if first == b'\r' && b.get(at) == Some(&b'\n') {
                    at += 1;
                }
                break;
            }
            let mut text = Vec::new();
            while at < b.len() && !matches!(b[at], b'\r' | b'\n') {
                let c = b[at];
                if c == b'\t' || (!tabs_only && c == b' ' && b.get(at + 1) != Some(&b' ')) {
                    at += 1;
                    if column + 1 < count {
                        columns[column + 1].clear();
                    }
                    break;
                }
                if c != b' ' {
                    text.push(c);
                }
                at += 1;
            }
            if column < count {
                columns[column] = text.into_iter().take_while(|v| *v != 0).collect();
                column += 1;
            }
        }
        if !tabs_only && columns[0] == b"END" {
            break;
        }
        rows.push(columns.clone());
    }
    rows
}
fn atoi(b: &[u8]) -> i32 {
    let s = String::from_utf8_lossy(b);
    let s = s.trim_start();
    let sign = if s.starts_with('-') { -1 } else { 1 };
    let s = s.strip_prefix(['-', '+']).unwrap_or(s);
    s.bytes().take_while(u8::is_ascii_digit).fold(0i32, |n, b| {
        n.saturating_mul(10).saturating_add((b - b'0') as i32)
    }) * sign
}
fn scene_reverb(b: &[u8]) -> CoreResult<Value> {
    if !b.len().is_multiple_of(8) || b.len() / 8 > 256 {
        return Err(fail("invalid scene reverb table size"));
    }
    let mut rows = Vec::new();
    for record in b.as_chunks::<8>().0 {
        rows.push(json!({"id":u16le(record,0)? as i16,"type":u16le(record,2)? as i16,"depth":u16le(record,4)? as i16}));
    }
    Ok(json!({"schema_version":1,"kind":"scene_reverb","rows":rows}))
}
fn scene_sound_routes(b: &[u8]) -> CoreResult<Value> {
    let mut rows = Vec::new();
    for columns in sound_rows_separated(b, 64, true) {
        if !columns[0].first().is_some_and(u8::is_ascii_digit) {
            continue;
        }
        let bank = |index: usize, prefix: u8| -> i16 {
            let value = &columns[index];
            if value.starts_with(b"*") {
                -2
            } else if value.first() == Some(&prefix) {
                atoi(value.get(3..).unwrap_or_default()) as i16
            } else {
                -1
            }
        };
        let environment = &columns[8];
        let mut env_bgm = 0;
        let mut env_vol = 0;
        let timed = b"\x8e\x9e\x8a\xd4\x95\xcf\x89\xbb*";
        if environment.starts_with(b"S") {
            env_bgm = atoi(environment.get(7..).unwrap_or_default()) as i16;
        } else if environment.starts_with(timed) {
            env_bgm = -1;
            let value = String::from_utf8_lossy(&environment[timed.len()..])
                .parse::<f64>()
                .unwrap_or(0.0) as f32;
            env_vol = ((127.0f32 * value) as i32).min(127) as i16;
        }
        let mut sources = [-1i16; 8];
        let mut column = 9;
        for index in 0..6 {
            let value = &columns[column];
            column += 1;
            if value.starts_with(b"*") {
                sources[0] = -2;
                break;
            }
            sources[index] = if value.starts_with(b"O") {
                atoi(value.get(3..).unwrap_or_default()) as i16
            } else {
                -1
            };
        }
        let event = &columns[column];
        column += 1;
        let event_se = if event.starts_with(b"E") {
            [
                atoi(event.get(3..6).unwrap_or_default()) as i16,
                atoi(event.get(7..10).unwrap_or_default()) as i16,
            ]
        } else {
            [-1, -1]
        };
        column += 5;
        let reverb = &columns[column];
        let reverb_id = if reverb.starts_with(b"R") {
            atoi(reverb.get(3..).unwrap_or_default()) as i16
        } else {
            -1
        };
        rows.push(json!({"id":atoi(&columns[0]) as i16,"bgm_no":bank(4,b'B'),"se_base":bank(5,b'B'),"se_battle":bank(6,b'F'),"se_env":bank(7,b'S'),"env_bgm":env_bgm,"env_vol":env_vol,"se_src":sources,"event_se":event_se,"reverb_id":reverb_id}));
    }
    if rows.len() > 512 {
        return Err(fail("scene sound routing table exceeds game capacity"));
    }
    Ok(json!({"schema_version":1,"kind":"scene_sound_routes","rows":rows}))
}
fn verify_scene_table(document: &Value, kind: &str) -> CoreResult<()> {
    let rows = document["rows"]
        .as_array()
        .ok_or_else(|| fail("scene audio rows missing"))?;
    let capacity = if kind == "scene_reverb" { 256 } else { 512 };
    if document["schema_version"] != 1 || document["kind"] != kind || rows.len() > capacity {
        return Err(fail("invalid scene audio table"));
    }
    let integer = |value: &Value| {
        value
            .as_i64()
            .is_some_and(|n| (i16::MIN as i64..=i16::MAX as i64).contains(&n))
    };
    for row in rows {
        let fields: &[&str] = if kind == "scene_reverb" {
            &["id", "type", "depth"]
        } else {
            &[
                "id",
                "bgm_no",
                "se_base",
                "se_battle",
                "se_env",
                "env_bgm",
                "env_vol",
                "reverb_id",
            ]
        };
        if fields.iter().any(|name| !integer(&row[*name])) {
            return Err(fail("invalid scene audio integer"));
        }
        if kind == "scene_sound_routes" {
            for (name, count) in [("se_src", 8), ("event_se", 2)] {
                if row[name].as_array().is_none_or(|values| {
                    values.len() != count || values.iter().any(|value| !integer(value))
                }) {
                    return Err(fail("invalid scene audio routing array"));
                }
            }
        }
    }
    Ok(())
}
fn sound_metadata_document(b: &[u8], volume: bool) -> CoreResult<Value> {
    let rows = sound_rows(b, if volume { 3 } else { 8 });
    let mut entries = Vec::new();
    let mut reverb = Vec::new();
    for row in &rows {
        if volume {
            if row[0] == b"REVERB" {
                reverb.push(json!({"type":atoi(&row[1]),"depth":atoi(&row[2])}));
            } else {
                entries.push(json!({"index":atoi(&row[0]),"volume":atoi(&row[1]) as i8}));
            }
        } else if row[0] == b"REVERBE" {
            let names = [
                "Off", "Room", "Studio_A", "Studio_B", "Studio_C", "Hall", "Space", "Echo",
                "Delay", "Pipe", "Max",
            ];
            let kind = names
                .iter()
                .position(|n| n.as_bytes() == row[1])
                .unwrap_or(0);
            reverb.push(json!({"type":kind,"depth":atoi(&row[2])}));
        } else if row[0].first().is_some_and(u8::is_ascii_digit) {
            let file = std::str::from_utf8(&row[4])
                .map_err(|_| fail("non-UTF8 sound sequence filename"))?;
            entries.push(json!({"filename":file,"program":atoi(&row[5]) as i8,"key":atoi(&row[6]) as i8,"flag":!row[7].is_empty()}));
        }
    }
    let kind = if volume {
        "sound_volume"
    } else {
        "sound_config"
    };
    let mut value = json!({"schema_version":1,"kind":kind,"entries":entries,"reverb":reverb});
    if !volume {
        value["entry_count"] = json!(rows.len());
    }
    Ok(value)
}

fn sound_metadata(root: &Path, key: &str, b: &[u8], volume: bool) -> CoreResult<String> {
    let document = path(key, if volume { "volume.json" } else { "config.json" });
    write_json(root, &document, &sound_metadata_document(b, volume)?)?;
    Ok(document)
}

/// Reconstructs all loose sound routing and volume settings from indexed retail extents.
pub fn gameplay_documents(
    source: &super::archive::SourceTree,
) -> CoreResult<BTreeMap<String, Value>> {
    let inventory = super::inventory::Inventory::build(source)?;
    let mut documents = BTreeMap::new();
    if let Some(resource) = inventory.get("snd2/rev.bin") {
        documents.insert("snd2/rev.json".to_owned(), scene_reverb(&resource.bytes())?);
    }
    if let Some(resource) = inventory.get("snd2/map_index.txt") {
        documents.insert(
            "snd2/map_index.json".to_owned(),
            scene_sound_routes(&resource.bytes())?,
        );
    }
    let mut sequence_names = BTreeMap::<String, BTreeMap<String, String>>::new();
    for resource in inventory.leaves() {
        let key = resource.alias.to_ascii_lowercase();
        let Some((group, member)) = key.split_once(".snd/") else {
            continue;
        };
        if member.ends_with(".sq") || member.ends_with(".mid") {
            sequence_names
                .entry(format!("{group}.snd"))
                .or_default()
                .entry(member.rsplit('/').next().unwrap_or(member).to_owned())
                .or_insert_with(|| path(&key, "sequence.json"));
        }
    }
    for resource in inventory.leaves() {
        let key = resource.alias.to_ascii_lowercase();
        if !owns_config(&key) {
            continue;
        }
        let volume = key.ends_with(".vol");
        let mut document = sound_metadata_document(&resource.bytes(), volume)?;
        if !volume {
            let (group, _) = key
                .split_once(".snd/")
                .ok_or_else(|| fail(format!("sound config outside bundle: {key}")))?;
            let names = sequence_names.get(&format!("{group}.snd"));
            for entry in document["entries"].as_array_mut().unwrap() {
                let original = entry["filename"]
                    .as_str()
                    .unwrap_or("")
                    .to_ascii_lowercase();
                if let Some(uri) = names.and_then(|names| names.get(&original)) {
                    entry["filename"] = json!(uri);
                } else if original.contains('.') {
                    entry["filename"] = json!("");
                }
            }
        }
        let output = path(&key, if volume { "volume.json" } else { "config.json" });
        if documents.insert(output.clone(), document).is_some() {
            return Err(fail(format!("duplicate sound gameplay document: {output}")));
        }
    }
    Ok(documents)
}

/// Updates only generated sound gameplay documents in explicit refresh mode.
pub fn export_gameplay(
    source: &super::archive::SourceTree,
    destination: &Path,
) -> CoreResult<usize> {
    let documents = gameplay_documents(source)?;
    for (name, document) in &documents {
        write_json(destination, name, document)?;
    }
    Ok(documents.len())
}

/// Compares loose sound gameplay documents with every retail source field.
pub fn audit_gameplay(
    source: &super::archive::SourceTree,
    destination: &Path,
) -> CoreResult<usize> {
    let documents = gameplay_documents(source)?;
    for (name, expected) in &documents {
        let actual: Value = serde_json::from_slice(&fs::read(destination.join(name))?)?;
        if &actual != expected {
            return Err(fail(format!(
                "sound gameplay differs from PAL source: {name}"
            )));
        }
    }
    Ok(documents.len())
}

/// Reads the PS2 SShd planar/interleaved PCM wrapper used by streams and movie private PES packets.
fn sshd(b: &[u8]) -> CoreResult<(Vec<i16>, u32, u16)> {
    if b.get(..4) != Some(b"SShd") {
        return Err(fail("missing SShd"));
    }
    let body = 8 + u32le(b, 4)? as usize;
    if b.get(body..body + 4) != Some(b"SSbd") || u32le(b, 8)? != 1 {
        return Err(fail("unsupported SShd codec"));
    }
    let rate = u32le(b, 12)?;
    let channels = u32le(b, 16)? as usize;
    let interleave = u32le(b, 20)? as usize;
    if !(1..=2).contains(&channels) || interleave == 0 || !interleave.is_multiple_of(2) {
        return Err(fail("invalid SShd layout"));
    }
    let bytes = u32le(b, body + 4)? as usize;
    let data = b
        .get(body + 8..body + 8 + bytes)
        .ok_or_else(|| fail("truncated SShd body"))?;
    let mut pcm = Vec::with_capacity(bytes / 2);
    for group in data.chunks(interleave * channels) {
        let stride = group.len() / channels;
        if !stride.is_multiple_of(2) {
            return Err(fail("unaligned SShd group"));
        }
        for frame in 0..stride / 2 {
            for channel in 0..channels {
                pcm.push(u16le(group, channel * stride + frame * 2)? as i16);
            }
        }
    }
    Ok((pcm, rate, channels as u16))
}
/// Converts RIFF PS2 ADPCM (format 0x69) or PCM into a canonical 16-bit WAV.
fn stream(b: &[u8]) -> CoreResult<Vec<u8>> {
    if b.starts_with(b"SShd") {
        let (p, r, c) = sshd(b)?;
        return wav(&p, r, c);
    }
    if b.get(..4) != Some(b"RIFF") || b.get(8..12) != Some(b"WAVE") {
        return Err(fail("unsupported sound stream container"));
    }
    let (mut at, mut fmt, mut data) = (12, None, None);
    while at + 8 <= b.len() {
        let n = u32le(b, at + 4)? as usize;
        let p = b
            .get(at + 8..at + 8 + n)
            .ok_or_else(|| fail("truncated WAV chunk"))?;
        if &b[at..at + 4] == b"fmt " {
            fmt = Some(p);
        }
        if &b[at..at + 4] == b"data" {
            data = Some(p);
        }
        at += 8 + n + (n & 1);
    }
    let f = fmt.ok_or_else(|| fail("WAV fmt missing"))?;
    let d = data.ok_or_else(|| fail("WAV data missing"))?;
    let codec = u16le(f, 0)?;
    let channels = u16le(f, 2)?;
    let rate = u32le(f, 4)?;
    if codec == 1 && u16le(f, 14)? == 16 {
        return wav(
            &d.as_chunks::<2>()
                .0
                .iter()
                .map(|s| i16::from_le_bytes(*s))
                .collect::<Vec<_>>(),
            rate,
            channels,
        );
    }
    if codec != 0x69 || !(1..=2).contains(&channels) {
        return Err(fail(format!("unsupported WAV codec {codec:#x}")));
    }
    let block_align = u16le(f, 12)? as usize;
    if block_align < 16 * channels as usize || !block_align.is_multiple_of(16 * channels as usize) {
        return Err(fail("invalid SPU WAV block alignment"));
    }
    // Each block contains one planar run per channel; predictor history continues between blocks.
    let stride = block_align / channels as usize;
    let mut planar = vec![Vec::new(); channels as usize];
    for group in d.chunks(block_align) {
        if group.len() % channels as usize != 0 {
            return Err(fail("truncated SPU WAV group"));
        }
        let count = group.len() / channels as usize;
        for c in 0..channels as usize {
            planar[c].extend(&group[c * count..(c + 1) * count]);
        }
    }
    let decoded = planar
        .iter()
        .map(|p| adpcm(p).map(|r| r.0))
        .collect::<CoreResult<Vec<_>>>()?;
    let frames = decoded.iter().map(Vec::len).min().unwrap_or(0);
    let mut pcm = Vec::new();
    for i in 0..frames {
        for p in &decoded {
            pcm.push(p[i]);
        }
    }
    let _ = stride;
    wav(&pcm, rate, channels)
}

fn movie(b: &[u8]) -> CoreResult<Option<(Vec<u8>, Value)>> {
    let (mut at, mut payload, mut first_video, mut first_pcm) = (0, Vec::new(), None, None);
    while at + 6 <= b.len() {
        if b.get(at..at + 3) != Some(&[0, 0, 1]) {
            at += 1;
            continue;
        }
        let id = b[at + 3];
        if id == 0xba {
            at += 4;
            continue;
        }
        if id == 0xb9 {
            break;
        }
        let n = u16::from_be_bytes([b[at + 4], b[at + 5]]) as usize;
        let p = b
            .get(at..at + 6 + n)
            .ok_or_else(|| fail("truncated movie PES"))?;
        if (id == 0xbd || id == 0xe0) && p.len() >= 9 {
            let data = 9 + p[8] as usize;
            if data > p.len() {
                return Err(fail("movie PES header overrun"));
            }
            let pts = if p[7] & 128 != 0 {
                let t = p.get(9..14).ok_or_else(|| fail("movie PTS truncated"))?;
                Some(
                    (((t[0] >> 1) & 7) as i64) << 30
                        | (t[1] as i64) << 22
                        | ((t[2] >> 1) as i64) << 15
                        | (t[3] as i64) << 7
                        | (t[4] >> 1) as i64,
                )
            } else {
                None
            };
            if id == 0xe0 {
                if first_video.is_none() {
                    first_video = pts;
                }
            } else if p.len() >= data + 4
                && p[data..data + 4].iter().copied().find(|v| *v != 255) == Some(0xa0)
            {
                if first_pcm.is_none() {
                    first_pcm = pts;
                }
                payload.extend(&p[data + 4..]);
            }
        }
        at += 6 + n;
    }
    if payload.is_empty() {
        return Ok(None);
    }
    let (mut pcm, rate, channels) = sshd(&payload)?;
    let offset = (first_pcm.unwrap_or(0) - first_video.unwrap_or(0)) * rate as i64 / 90000;
    if offset > 0 {
        let mut padded = vec![0; offset as usize * channels as usize];
        padded.extend(pcm);
        pcm = padded;
    } else if offset < 0 {
        let drop = ((-offset) as usize * channels as usize).min(pcm.len());
        pcm.drain(..drop);
    }
    Ok(Some((
        wav(&pcm, rate, channels)?,
        json!({"first_video_pts":first_video,"first_audio_pts":first_pcm,"clock_hz":90000,"offset_frames":offset}),
    )))
}

fn movie_target(key: &str) -> String {
    let name = key
        .strip_prefix("movie/")
        .unwrap_or(key)
        .trim_end_matches(".pss");
    format!("movie/{name}.mp4")
}

/// Exports representative retail and converted movies for explicit, post-extraction parity checks.
pub fn export_movie_parity_fixture(
    source: &super::archive::SourceTree,
    archive_path: &Path,
    destination: &Path,
) -> CoreResult<usize> {
    let archive = super::zip_archive::Archive::open(archive_path)?;
    let directory = destination.join("movies");
    fs::create_dir_all(&directory)?;
    let mut selected_audio = false;
    let mut selected_silent = false;
    let mut cases = Vec::new();
    for alias in source
        .names()
        .filter(|alias| alias.to_ascii_lowercase().ends_with(".pss"))
    {
        let original = source.read(alias)?;
        let decoded = movie(&original)?;
        if decoded.is_some() && selected_audio || decoded.is_none() && selected_silent {
            continue;
        }
        let video = strip_movie_audio(&original)?;
        let target = movie_target(alias);
        let index = (0..archive.len())
            .find(|index| archive.name(*index) == target)
            .ok_or_else(|| fail(format!("converted movie missing from archive: {target}")))?;
        let converted = archive.read(index)?;
        let prefix = format!("movie_{}", cases.len());
        let video_name = format!("{prefix}.video.ps");
        let output_name = format!("{prefix}.mp4");
        fs::write(directory.join(&video_name), video)?;
        fs::write(directory.join(&output_name), converted)?;
        let (wav_name, alignment) = if let Some((wav, alignment)) = decoded {
            let name = format!("{prefix}.wav");
            fs::write(directory.join(&name), wav)?;
            selected_audio = true;
            (Some(name), alignment)
        } else {
            selected_silent = true;
            (None, Value::Null)
        };
        cases.push(
            json!({"source_alias":alias,"archive_alias":target,"video":video_name,
            "mp4":output_name,"wav":wav_name,"alignment":alignment}),
        );
        if selected_audio && selected_silent {
            break;
        }
    }
    if !selected_audio {
        return Err(fail("no PAL movie with native audio for parity"));
    }
    let count = cases.len();
    let mut manifest = serde_json::to_vec_pretty(
        &json!({"schema_version":1,"kind":"movie_parity","cases":cases}),
    )?;
    manifest.push(b'\n');
    fs::write(directory.join("manifest.json"), manifest)?;
    Ok(count)
}

#[cfg(kazarov_ffmpeg)]
unsafe extern "C" {
    fn kazarov_mux_movie(
        video: *const u8,
        video_size: usize,
        wav: *const u8,
        wav_size: usize,
        result_bytes: *mut *mut u8,
        result_size: *mut usize,
        error: *mut std::ffi::c_char,
        error_size: usize,
    ) -> i32;
    fn kazarov_free_movie(buffer: *mut u8);
}

/// Muxes the original MPEG-2 video and lossless ALAC audio through seekable memory AVIO.
#[cfg(kazarov_ffmpeg)]
fn movie_mp4_memory(source: &[u8]) -> CoreResult<Vec<u8>> {
    let video = strip_movie_audio(source)?;
    let audio = movie(source)?.map(|(wav, _)| wav);
    let mut bytes = std::ptr::null_mut();
    let mut length = 0usize;
    let mut error = [0 as std::ffi::c_char; 256];
    let (wav, wav_size) = audio
        .as_ref()
        .map_or((std::ptr::null(), 0), |wav| (wav.as_ptr(), wav.len()));
    let status = unsafe {
        kazarov_mux_movie(
            video.as_ptr(),
            video.len(),
            wav,
            wav_size,
            &mut bytes,
            &mut length,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if status < 0 {
        let message = unsafe { std::ffi::CStr::from_ptr(error.as_ptr()) }.to_string_lossy();
        return Err(fail(format!(
            "in-memory FFmpeg movie mux failed: {message}"
        )));
    }
    if bytes.is_null() || length == 0 {
        return Err(fail("in-memory movie mux returned no MP4 bytes"));
    }
    let result = unsafe { std::slice::from_raw_parts(bytes, length).to_vec() };
    unsafe { kazarov_free_movie(bytes) };
    Ok(result)
}

#[cfg(not(kazarov_ffmpeg))]
fn movie_mp4_memory(_: &[u8]) -> CoreResult<Vec<u8>> {
    Err(fail(
        "in-memory movie mux requires FFmpeg development libraries",
    ))
}

/// Asset conversion jobs and retail audio resources removed from the retained PACK stream.
pub struct AudioPlan {
    pub jobs: Vec<Job>,
    pub consumed: HashSet<String>,
}

fn submit_json(sink: &AssetSink, name: &str, document: &Value) -> CoreResult<()> {
    sink.submit(
        name.to_owned(),
        ArtifactBytes::Owned(serde_json::to_vec(document)?),
        Category::Audio,
    )
}

/// Plans sound and movie conversion without copying any retail source bytes.
pub fn plan_stream(inventory: &Inventory) -> CoreResult<AudioPlan> {
    let mut jobs = Vec::new();
    let mut consumed = HashSet::new();
    let mut leaves = BTreeMap::<String, ResourceRef>::new();
    let mut groups = BTreeMap::<String, String>::new();
    for resource in inventory.leaves() {
        let key = resource.alias.to_ascii_lowercase();
        if let Some((base, _)) = key.split_once(".snd/") {
            let lower = format!("{base}.snd");
            groups
                .entry(lower)
                .or_insert_with(|| resource.alias[..base.len() + 4].to_owned());
        }
        leaves.insert(key, resource.clone());
    }
    for (key, resource) in &leaves {
        let owned = matches!(
            key.as_str(),
            "sound/sound.dat" | "sound/sound.hd3" | "snd2/rev.bin" | "snd2/map_index.txt"
        ) || key.ends_with(".snd")
            || key.ends_with(".hd")
            || key.ends_with(".bd")
            || key.ends_with(".sq")
            || key.ends_with(".mid")
            || key.ends_with(".pss")
            || owns_config(key)
            || key.ends_with(".vol")
            || key.ends_with(".wav")
            || resource.bytes().starts_with(b"SShd")
            || (resource.bytes().starts_with(b"RIFF")
                && resource.bytes().get(8..12) == Some(b"WAVE"));
        if !owned {
            continue;
        }
        consumed.insert(resource.alias.clone());
        if matches!(
            key.as_str(),
            "sound/sound.dat" | "sound/sound.hd3" | "snd2/rev.bin" | "snd2/map_index.txt"
        ) || owns_config(key)
            || key.ends_with(".vol")
        {
            continue;
        }
        if key.ends_with(".hd") {
            let bd_key = format!("{}.bd", key.trim_end_matches(".hd"));
            let bd = leaves
                .get(&bd_key)
                .or_else(|| {
                    leaves
                        .iter()
                        .find(|(name, _)| {
                            Path::new(name).parent() == Path::new(key).parent()
                                && name.ends_with(".bd")
                        })
                        .map(|(_, resource)| resource)
                })
                .ok_or_else(|| fail(format!("bank BD companion missing: {key}")))?
                .clone();
            let key = key.clone();
            let hd = resource.clone();
            jobs.push(Job {
                category: Category::Audio,
                label: key.clone(),
                work: (hd.len() + bd.len()) as u64,
                estimated_bytes: bd.len().saturating_mul(4).saturating_add(hd.len()),
                run: Box::new(move |sink| {
                    bank_with(&key, &hd.bytes(), &bd.bytes(), &mut |name, bytes| {
                        sink.submit(
                            name.to_owned(),
                            ArtifactBytes::Owned(bytes),
                            Category::Audio,
                        )
                    })?;
                    Ok(())
                }),
            });
        } else if key.ends_with(".sq") || key.ends_with(".mid") {
            let key = key.clone();
            let input = resource.clone();
            jobs.push(Job {
                category: Category::Audio,
                label: key.clone(),
                work: input.len() as u64,
                estimated_bytes: input.len().saturating_mul(4),
                run: Box::new(move |sink| {
                    let mut emit = |name: &str, bytes| {
                        sink.submit(
                            name.to_owned(),
                            ArtifactBytes::Owned(bytes),
                            Category::Audio,
                        )
                    };
                    if key.ends_with(".sq") {
                        sequence_with(&key, &input.bytes(), &mut emit)?;
                    } else {
                        smf_with(&key, &input.bytes(), &mut emit)?;
                    }
                    Ok(())
                }),
            });
        } else if key.ends_with(".pss") {
            let key = key.clone();
            let input = resource.clone();
            jobs.push(Job {
                category: Category::Movie,
                label: key.clone(),
                work: input.len() as u64,
                estimated_bytes: input.len().saturating_mul(3),
                run: Box::new(move |sink| {
                    let mp4 = movie_mp4_memory(&input.bytes())?;
                    sink.submit(
                        movie_target(&key),
                        ArtifactBytes::Owned(mp4),
                        Category::Movie,
                    )
                }),
            });
        } else if key.ends_with(".wav")
            || resource.bytes().starts_with(b"SShd")
            || (resource.bytes().starts_with(b"RIFF")
                && resource.bytes().get(8..12) == Some(b"WAVE"))
        {
            let key = key.clone();
            let input = resource.clone();
            jobs.push(Job {
                category: Category::Audio,
                label: key.clone(),
                work: input.len() as u64,
                estimated_bytes: input.len().saturating_mul(3),
                run: Box::new(move |sink| {
                    sink.submit(
                        format!("{}.wav", key.trim_end_matches(".wav")),
                        ArtifactBytes::Owned(
                            stream(&input.bytes())
                                .map_err(|error| fail(format!("{key}: {error}")))?,
                        ),
                        Category::Audio,
                    )
                }),
            });
        }
    }
    for (group, original_group) in groups {
        let members = inventory
            .members(&original_group)
            .ok_or_else(|| fail(format!("sound bundle is not a PACK: {group}")))?;
        let mut bank = Value::Null;
        let mut config = Value::Null;
        let mut volume = Value::Null;
        let mut sequences = Vec::new();
        let mut effects = Vec::new();
        for member in members {
            let key = member.alias.to_ascii_lowercase();
            if key.ends_with(".hd") {
                bank = json!(path(&key, "bank.json"));
            }
            if key.ends_with(".cfg") {
                config = json!(path(&key, "config.json"));
            }
            if key.ends_with(".vol") {
                volume = json!(path(&key, "volume.json"));
            }
            if key.ends_with(".sq") {
                sequences.push(path(&key, "sequence.json"));
            }
            if key.ends_with(".mid") {
                effects.push(path(&key, "sequence.json"));
            }
        }
        if config.is_null() || volume.is_null() {
            return Err(fail(format!("{group}: sound controls missing")));
        }
        let output = format!("{}.sound.json", group.trim_end_matches(".snd"));
        let document = json!({"schema_version":1,"kind":"sound_bundle","bank":bank,"sequences":sequences,"effect_sequences":effects,"config":config,"volume":volume});
        jobs.push(Job {
            category: Category::Audio,
            label: group.clone(),
            work: 4096,
            estimated_bytes: 4096,
            run: Box::new(move |sink| submit_json(sink, &output, &document)),
        });
        consumed.insert(original_group);
    }
    Ok(AudioPlan { jobs, consumed })
}

static FFMPEG_SLOTS: (Mutex<usize>, Condvar) = (Mutex::new(0), Condvar::new());

struct FfmpegSlot;

impl FfmpegSlot {
    fn acquire() -> Self {
        let (running, released) = &FFMPEG_SLOTS;
        let limit = rayon::current_num_threads().clamp(1, 4);
        let mut running = running.lock().unwrap_or_else(PoisonError::into_inner);
        while *running >= limit {
            running = released
                .wait(running)
                .unwrap_or_else(PoisonError::into_inner);
        }
        *running += 1;
        Self
    }
}

impl Drop for FfmpegSlot {
    fn drop(&mut self) {
        let (running, released) = &FFMPEG_SLOTS;
        *running.lock().unwrap_or_else(PoisonError::into_inner) -= 1;
        released.notify_one();
    }
}

fn movie_mp4(root: &Path, key: &str, b: &[u8]) -> CoreResult<String> {
    let target = movie_target(key);
    write(root, &target, &movie_mp4_memory(b)?)?;
    Ok(target)
}

fn pack(b: &[u8]) -> Option<Vec<(String, &[u8])>> {
    if super::native_scripts::is_valid_script(b) {
        return None;
    }
    let mut out = Vec::new();
    let mut cursor = 0;
    let mut seen = BTreeSet::new();
    while seen.insert(cursor) && out.len() < 16384 {
        let e = b.get(cursor..cursor + 76)?;
        let end = e[..64].iter().position(|v| *v == 0)?;
        if end == 0 {
            return if out.is_empty() { None } else { Some(out) };
        }
        if e[..end]
            .iter()
            .any(|v| *v < 32 || *v == b'/' || *v == b'\\')
        {
            return None;
        }
        let offset = u32le(e, 64).ok()? as usize;
        let size = u32le(e, 68).ok()? as usize;
        let advance = u32le(e, 72).ok()? as usize;
        if offset < 76 || size == 0 || advance < 76 {
            return None;
        }
        let name = String::from_utf8(e[..end].to_ascii_lowercase()).ok()?;
        out.push((name, b.get(cursor + offset..cursor + offset + size)?));
        cursor = cursor.checked_add(advance)?;
    }
    None
}
fn files_under(dir: &Path, out: &mut Vec<PathBuf>) -> CoreResult<()> {
    for e in fs::read_dir(dir)? {
        let p = e?.path();
        if p.is_dir() {
            files_under(&p, out)?;
        } else {
            out.push(p);
        }
    }
    Ok(())
}
#[cfg(test)]
fn collect(raw: &Path, phase: &Phase) -> CoreResult<BTreeMap<String, Vec<u8>>> {
    collect_source(&super::archive::SourceTree::directory(raw)?, phase)
}

fn source_sidecar(source: &super::archive::SourceTree, key: &str) -> bool {
    let path = Path::new(key);
    ["cfg", "vol"].contains(&path.extension().and_then(|s| s.to_str()).unwrap_or(""))
        && ["hd", "sq", "mid"].iter().any(|ext| {
            source.contains(
                &path
                    .with_extension(ext)
                    .to_string_lossy()
                    .replace('\\', "/"),
            )
        })
}

/// True when conversion replaces a complete retail source file.
pub fn owns_source(source: &super::archive::SourceTree, key: &str) -> CoreResult<bool> {
    let lower = key.to_ascii_lowercase();
    if matches!(
        lower.as_str(),
        "sound/sound.dat" | "sound/sound.hd3" | "snd2/rev.bin" | "snd2/map_index.txt"
    ) {
        return Ok(true);
    }
    if ["snd", "hd", "bd", "sq", "mid", "wav", "pss"]
        .contains(&lower.rsplit('.').next().unwrap_or(""))
        || source_sidecar(source, key)
    {
        return Ok(true);
    }
    Ok(false)
}

fn collect_source(
    source: &super::archive::SourceTree,
    phase: &Phase,
) -> CoreResult<BTreeMap<String, Vec<u8>>> {
    let mut sources = Vec::new();
    for name in source.names() {
        let key = name.to_ascii_lowercase();
        if key == "sound/sound.dat" || key == "sound/sound.hd3" || key.ends_with(".pss") {
            continue;
        }
        let sidecar = source_sidecar(source, name);
        sources.push((key, name, sidecar));
    }
    sources.sort();
    let parts = sources
        .par_iter()
        .map(
            |(key, name, sidecar)| -> CoreResult<BTreeMap<String, Vec<u8>>> {
                let b = source.read(name)?;
                phase.inc(1);
                let mut local = BTreeMap::new();
                if *sidecar {
                    local.insert(key.clone(), b);
                } else {
                    expand(key, b, &mut local, 0)?;
                }
                Ok(local)
            },
        )
        .collect::<Vec<_>>();
    let mut files = BTreeMap::new();
    for part in parts {
        files.extend(part?);
    }
    Ok(files)
}
/// Relative output paths that converting `key` with contents `b` writes under data.
fn outputs(key: &str, b: &[u8]) -> Vec<String> {
    if key == "snd2/rev.bin" {
        vec!["snd2/rev.json".into()]
    } else if key == "snd2/map_index.txt" {
        vec!["snd2/map_index.json".into()]
    } else if key.ends_with(".hd") {
        let vags = chunk(b, b"Vagi").and_then(entries).unwrap_or_default();
        let mut out = vec![path(key, "bank.json")];
        out.extend(
            vags.iter()
                .enumerate()
                .filter(|(_, p)| !p.is_empty())
                .map(|(i, _)| path(key, &format!("sample-{i}.wav"))),
        );
        out
    } else if key.ends_with(".sq") || key.ends_with(".mid") {
        vec![path(key, "sequence.json")]
    } else if key.ends_with(".cfg") {
        vec![path(key, "config.json")]
    } else if key.ends_with(".vol") {
        vec![path(key, "volume.json")]
    } else if key.ends_with(".wav") || b.starts_with(b"SShd") || b.starts_with(b"RIFF") {
        vec![format!("{}.wav", key.trim_end_matches(".wav"))]
    } else if key.ends_with(".pss") {
        vec![movie_target(key)]
    } else {
        vec![]
    }
}
fn expand(
    key: &str,
    b: Vec<u8>,
    files: &mut BTreeMap<String, Vec<u8>>,
    depth: usize,
) -> CoreResult<()> {
    if depth > 8 {
        return Err(fail("audio PACK nesting exceeds 8"));
    }
    if let Some(members) = pack(&b) {
        let sound_group = members.iter().any(|(name, _)| {
            ["hd", "bd", "sq", "mid"].contains(&name.rsplit('.').next().unwrap_or(""))
        });
        let mut seen = BTreeSet::new();
        for (i, (name, bytes)) in members.into_iter().enumerate() {
            let child = if seen.insert(name.clone()) {
                format!("{key}/{name}")
            } else {
                format!("{key}/{name}#entry:{i}")
            };
            if sound_group && (name.ends_with(".cfg") || name.ends_with(".vol")) {
                files.insert(child, bytes.to_vec());
            } else {
                expand(&child, bytes.to_vec(), files, depth + 1)?;
            }
        }
    } else if matches!(key, "snd2/rev.bin" | "snd2/map_index.txt")
        || ["hd", "bd", "sq", "mid", "wav", "pss"].contains(&key.rsplit('.').next().unwrap_or(""))
        || b.starts_with(b"SShd")
        || (b.starts_with(b"RIFF") && b.get(8..12) == Some(b"WAVE"))
    {
        files.insert(key.into(), b);
    }
    Ok(())
}

fn read_source(source: &super::archive::SourceTree, key: &str) -> CoreResult<Vec<u8>> {
    let components: Vec<_> = key.split('/').collect();
    let mut physical = String::new();
    for (i, component) in components.iter().enumerate() {
        if !physical.is_empty() {
            physical.push('/');
        }
        physical.push_str(component);
        if !source.contains(&physical) {
            continue;
        }
        let mut bytes = source.read(&physical)?;
        for name in &components[i + 1..] {
            let members =
                pack(&bytes).ok_or_else(|| fail(format!("source is not an archive: {key}")))?;
            let found = members
                .into_iter()
                .find(|(member, _)| member == name)
                .ok_or_else(|| fail(format!("source member missing: {key}")))?;
            bytes = found.1.to_vec();
        }
        return Ok(bytes);
    }
    Err(fail(format!("source file missing: {key}")))
}

/// Result of converting one source key, applied to the shared maps in key order.
enum Outcome {
    None,
    Bank(String, String, Value),
    Sequence(String, Value),
    Config(String, Value),
    Volume(String, Value),
    Stream(String, Value),
    Movie(String, String),
}

/// Planned sound bundle document and the config rewritten alongside it.
struct Bundle {
    config_path: String,
    config: Value,
    path: String,
    document: Value,
}

/// Publishes directly addressed sound bundles, recordings, and self-contained movies, then strips them.
pub fn convert_tree(raw: &Path, published: &Path, _jobs: usize) -> CoreResult<()> {
    let source = super::archive::SourceTree::directory(raw)?;
    convert_source(&source, published, &Phase::noop())?;
    strip_source(&source, published, &Phase::noop())
}

/// Publishes directly addressed sound bundles, recordings, and self-contained movies into data.
pub fn convert_assets(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    convert_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        phase,
    )
}

pub fn convert_source(
    source: &super::archive::SourceTree,
    published: &Path,
    phase: &Phase,
) -> CoreResult<()> {
    let source_count = source.names().count() as u64;
    let movie_count = source
        .names()
        .filter(|name| name.to_ascii_lowercase().ends_with(".pss"))
        .count() as u64;
    // Expansion of nested sound members varies by disc; reserve the observed
    // work of two passes and complete the remaining budget when publication ends.
    let total = source_count
        .saturating_mul(2)
        .saturating_add(movie_count)
        .max(1);
    phase.set_total(total);
    let assets = published.join("data");
    let published_assets = assets.as_path();
    let files = collect_source(source, phase)?;
    let (mut banks, mut sequences, mut streams, mut movies) = (
        BTreeMap::new(),
        BTreeMap::new(),
        BTreeMap::new(),
        BTreeMap::new(),
    );
    let (mut configs, mut volumes) = (BTreeMap::new(), BTreeMap::new());
    let convert = |(key, b): (&String, &Vec<u8>)| -> CoreResult<Outcome> {
        let result = (|| -> CoreResult<Outcome> {
            if key == "snd2/rev.bin" {
                write_json(published_assets, "snd2/rev.json", &scene_reverb(b)?)?;
            } else if key == "snd2/map_index.txt" {
                write_json(
                    published_assets,
                    "snd2/map_index.json",
                    &scene_sound_routes(b)?,
                )?;
            } else if key.ends_with(".hd") {
                let bd_key = format!("{}.bd", key.trim_end_matches(".hd"));
                let bd = files
                    .get(&bd_key)
                    .or_else(|| {
                        files
                            .iter()
                            .find(|(k, _)| {
                                Path::new(k).parent() == Path::new(key).parent()
                                    && k.ends_with(".bd")
                            })
                            .map(|(_, v)| v)
                    })
                    .ok_or_else(|| fail("bank BD companion missing"))?;
                let document = Value::String(bank(published_assets, key, b, bd)?);
                return Ok(Outcome::Bank(key.clone(), bd_key, document));
            } else if key.ends_with(".sq") {
                return Ok(Outcome::Sequence(
                    key.clone(),
                    Value::String(sequence(published_assets, key, b)?),
                ));
            } else if key.ends_with(".mid") {
                return Ok(Outcome::Sequence(
                    key.clone(),
                    Value::String(smf(published_assets, key, b)?),
                ));
            } else if key.ends_with(".cfg") {
                return Ok(Outcome::Config(
                    key.clone(),
                    Value::String(sound_metadata(published_assets, key, b, false)?),
                ));
            } else if key.ends_with(".vol") {
                return Ok(Outcome::Volume(
                    key.clone(),
                    Value::String(sound_metadata(published_assets, key, b, true)?),
                ));
            } else if key.ends_with(".wav") || b.starts_with(b"SShd") || b.starts_with(b"RIFF") {
                phase.item(key);
                let target = format!("{}.wav", key.trim_end_matches(".wav"));
                write(published_assets, &target, &stream(b)?)?;
                return Ok(Outcome::Stream(key.clone(), Value::String(target)));
            } else if key.ends_with(".pss") {
                phase.item(key);
                let _slot = FfmpegSlot::acquire();
                return Ok(Outcome::Movie(
                    key.clone(),
                    movie_mp4(published_assets, key, b)?,
                ));
            }
            Ok(Outcome::None)
        })();
        phase.inc(1);
        result.map_err(|e| fail(format!("{key}: {e}")))
    };
    let mut owners: BTreeMap<String, &str> = BTreeMap::new();
    let disjoint = files.iter().all(|(key, b)| {
        outputs(key, b).into_iter().all(|out| {
            owners
                .insert(out, key.as_str())
                .is_none_or(|owner| owner == key.as_str())
        })
    });
    let outcomes: Vec<CoreResult<Outcome>> = if disjoint {
        files.par_iter().map(convert).collect()
    } else {
        files
            .iter()
            .map(convert)
            .scan(false, |failed, outcome| {
                if *failed {
                    return None;
                }
                *failed = outcome.is_err();
                Some(outcome)
            })
            .collect()
    };
    for outcome in outcomes {
        match outcome? {
            Outcome::None => {}
            Outcome::Bank(key, bd_key, document) => {
                banks.insert(key, document.clone());
                banks.insert(bd_key, document);
            }
            Outcome::Sequence(key, document) => {
                sequences.insert(key, document);
            }
            Outcome::Config(key, document) => {
                configs.insert(key, document);
            }
            Outcome::Volume(key, document) => {
                volumes.insert(key, document);
            }
            Outcome::Stream(key, document) => {
                streams.insert(key, document);
            }
            Outcome::Movie(key, document) => {
                movies.insert(key, document);
            }
        }
    }
    let direct_movies: Vec<&str> = source
        .names()
        .filter(|name| name.to_ascii_lowercase().ends_with(".pss"))
        .collect();
    let converted_movies: Vec<CoreResult<(String, String)>> = direct_movies
        .par_iter()
        .map(|name| {
            let _slot = FfmpegSlot::acquire();
            let bytes = source.read(name)?;
            let target = movie_mp4(published_assets, name, &bytes)?;
            phase.inc(1);
            Ok((name.to_string(), target))
        })
        .collect();
    for movie in converted_movies {
        let (name, target) = movie?;
        movies.insert(name, target);
    }
    let mut groups = BTreeSet::new();
    for key in files.keys() {
        if let Some((prefix, _)) = key.split_once(".snd/") {
            groups.insert(format!("{prefix}.snd"));
        }
    }
    let plan_group = |group: &String| -> CoreResult<Bundle> {
        let original = read_source(source, group)?;
        let members = pack(&original).ok_or_else(|| fail("sound bundle is not a PACK"))?;
        let mut sequence_refs = Vec::new();
        let mut effect_refs = Vec::new();
        let mut bank_ref = Value::Null;
        let mut config_ref = Value::Null;
        let mut volume_ref = Value::Null;
        let mut names = BTreeMap::new();
        let mut seen = BTreeSet::new();
        for (i, (name, _)) in members.into_iter().enumerate() {
            let key = if seen.insert(name.clone()) {
                format!("{group}/{name}")
            } else {
                format!("{group}/{name}#entry:{i}")
            };
            if let Some(uri) = banks.get(&key) {
                bank_ref = uri.clone();
            }
            if let Some(uri) = configs.get(&key) {
                config_ref = uri.clone();
            }
            if let Some(uri) = volumes.get(&key) {
                volume_ref = uri.clone();
            }
            if let Some(uri) = sequences.get(&key) {
                names.entry(name.clone()).or_insert_with(|| uri.clone());
                if name.ends_with(".sq") {
                    sequence_refs.push(uri.clone());
                }
                if name.ends_with(".mid") {
                    effect_refs.push(uri.clone());
                }
            }
        }
        if config_ref.is_null() || volume_ref.is_null() {
            return Err(fail(format!("{group}: sound controls missing")));
        }
        let config_path = config_ref.as_str().unwrap().to_owned();
        let mut config: Value = serde_json::from_slice(&fs::read(assets.join(&config_path))?)?;
        for entry in config["entries"].as_array_mut().unwrap() {
            if let Some(uri) = entry["filename"]
                .as_str()
                .and_then(|n| names.get(&n.to_ascii_lowercase()))
            {
                entry["filename"] = uri.clone();
            } else if entry["filename"]
                .as_str()
                .is_some_and(|name| name.contains('.'))
            {
                entry["filename"] = json!("");
            }
        }
        let path = format!("{}.sound.json", group.trim_end_matches(".snd"));
        let document = json!({"schema_version":1,"kind":"sound_bundle","bank":bank_ref,"sequences":sequence_refs,"effect_sequences":effect_refs,"config":config_ref,"volume":volume_ref});
        Ok(Bundle {
            config_path,
            config,
            path,
            document,
        })
    };
    let group_refs: Vec<&String> = groups.iter().collect();
    let bundles: Vec<CoreResult<Bundle>> = group_refs
        .par_iter()
        .map(|group| plan_group(group))
        .collect();
    let disjoint = bundles.iter().all(Result::is_ok) && {
        let mut written = BTreeSet::new();
        bundles
            .iter()
            .flatten()
            .all(|bundle| written.insert(&bundle.config_path) && written.insert(&bundle.path))
    };
    let write_bundle = |bundle: &Bundle| -> CoreResult<()> {
        write_json(&assets, &bundle.config_path, &bundle.config)?;
        write_json(&assets, &bundle.path, &bundle.document)
    };
    if disjoint {
        bundles
            .into_par_iter()
            .map(|bundle| -> CoreResult<()> {
                write_bundle(&bundle?)?;
                phase.inc(1);
                Ok(())
            })
            .collect::<Vec<_>>()
            .into_iter()
            .collect::<CoreResult<()>>()?;
    } else {
        for group in &group_refs {
            write_bundle(&plan_group(group)?)?;
            phase.inc(1);
        }
    }
    phase.set_done(total);
    Ok(())
}

/// Removes or zeros the audio payloads `convert_assets` published from the data tree.
pub fn strip_data(raw: &Path, published: &Path, phase: &Phase) -> CoreResult<()> {
    strip_source(
        &super::archive::SourceTree::directory(raw)?,
        published,
        phase,
    )
}

pub fn strip_source(
    source: &super::archive::SourceTree,
    published: &Path,
    phase: &Phase,
) -> CoreResult<()> {
    let mut found: Vec<&str> = source.names().collect();
    found.sort();
    phase.add_total(found.len() as u64);
    let output = published.join("data");
    found
        .par_iter()
        .map(|p| {
            let result = strip_one(source, p, &output);
            phase.inc(1);
            result
        })
        .collect::<Vec<_>>()
        .into_iter()
        .collect()
}

fn strip_one(source: &super::archive::SourceTree, p: &str, output: &Path) -> CoreResult<()> {
    let key = p.to_ascii_lowercase();
    let published = output
        .parent()
        .ok_or_else(|| fail("missing published root"))?;
    let destination = super::archive::compatibility_path(published, p);
    if matches!(
        key.as_str(),
        "sound/sound.dat" | "sound/sound.hd3" | "snd2/rev.bin" | "snd2/map_index.txt"
    ) {
        if destination.is_file() {
            fs::remove_file(destination)?;
        }
        return Ok(());
    }
    if ["snd", "hd", "bd", "sq", "mid", "pss"].contains(&key.rsplit('.').next().unwrap_or("")) {
        if destination.is_file() {
            fs::remove_file(destination)?;
        }
        return Ok(());
    }
    if source_sidecar(source, p) {
        if destination.is_file() {
            fs::remove_file(destination)?;
        }
        return Ok(());
    }
    if key.ends_with(".wav") {
        // The converted PCM stream replaces this source at the same path.
        return Ok(());
    }
    // Model and texture passes may already have removed a whole converted source.
    if !destination.is_file() {
        return Ok(());
    }
    let original = source.read(p)?;
    if original.starts_with(b"SShd")
        || (original.starts_with(b"RIFF") && original.get(8..12) == Some(b"WAVE"))
    {
        fs::remove_file(destination)?;
        return Ok(());
    }
    if pack(&original).is_some() {
        let mut current = fs::read(&destination)?;
        strip_pack(&original, &mut current, 0)?;
        fs::write(destination, current)?;
    }
    Ok(())
}

fn strip_pack(original: &[u8], current: &mut [u8], depth: usize) -> CoreResult<()> {
    if depth > 8 {
        return Err(fail("audio PACK nesting exceeds 8"));
    }
    let Some(members) = pack(original) else {
        return Ok(());
    };
    let sound_group = members.iter().any(|(name, _)| {
        ["hd", "bd", "sq", "mid"].contains(&name.rsplit('.').next().unwrap_or(""))
    });
    let mut cursor = 0;
    for (name, b) in members {
        let offset = u32le(original, cursor + 64)? as usize;
        if ["snd", "hd", "bd", "sq", "mid", "wav"].contains(&name.rsplit('.').next().unwrap_or(""))
            || (sound_group && (name.ends_with(".cfg") || name.ends_with(".vol")))
            || b.starts_with(b"SShd")
            || (b.starts_with(b"RIFF") && b.get(8..12) == Some(b"WAVE"))
        {
            current
                .get_mut(cursor + 68..cursor + 72)
                .ok_or_else(|| fail("published PACK header missing"))?
                .fill(0);
            current
                .get_mut(cursor + offset..cursor + offset + b.len())
                .ok_or_else(|| fail("published PACK payload missing"))?
                .fill(0);
        } else if pack(b).is_some() && u32le(current, cursor + 68)? as usize == b.len() {
            strip_pack(
                b,
                &mut current[cursor + offset..cursor + offset + b.len()],
                depth + 1,
            )?;
        }
        cursor += u32le(original, cursor + 72)? as usize;
    }
    Ok(())
}
fn strip_movie_audio(b: &[u8]) -> CoreResult<Vec<u8>> {
    let mut out = Vec::with_capacity(b.len());
    let mut at = 0;
    while at + 6 <= b.len() {
        if b.get(at..at + 4) == Some(&[0, 0, 1, 0xbd]) {
            let n = 6 + u16::from_be_bytes([b[at + 4], b[at + 5]]) as usize;
            if at + n > b.len() {
                return Err(fail("truncated private movie PES"));
            }
            at += n;
        } else {
            out.push(b[at]);
            at += 1;
        }
    }
    out.extend(&b[at..]);
    Ok(out)
}

/// Audits directly addressed native audio resources and their dependencies.
pub fn verify(root: &Path) -> CoreResult<()> {
    let assets = root.join("data");
    if !assets.is_dir() {
        return Ok(());
    }
    let mut found = Vec::new();
    files_under(&assets, &mut found)?;
    let names: BTreeSet<String> = found
        .iter()
        .map(|path| {
            path.strip_prefix(&assets)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/")
        })
        .collect();
    verify_files(root, &names, &Phase::noop())
}

/// Counts audio inventory and semantic checks in a staged data tree.
pub fn verify_work(names: &BTreeSet<String>) -> u64 {
    let candidates = names.iter().filter(|name| {
        name.ends_with(".wav")
            || name.ends_with(".mp4")
            || name.ends_with(".sound.json")
            || name.contains(".sound/") && name.ends_with(".json")
    });
    names.len() as u64 * 2 + candidates.count() as u64
}

/// Verifies audio against a previously collected data-tree inventory.
pub fn verify_files(root: &Path, names: &BTreeSet<String>, phase: &Phase) -> CoreResult<()> {
    let assets = root.join("data");
    if !assets.is_dir() {
        return Ok(());
    }
    struct DirectoryView(PathBuf, Vec<String>);
    impl super::publish::ArchiveReader for DirectoryView {
        fn names(&self) -> Vec<String> {
            self.1.clone()
        }
        fn contains(&self, name: &str) -> bool {
            self.0.join(name).is_file()
        }
        fn read(&self, name: &str) -> CoreResult<Vec<u8>> {
            Ok(fs::read(self.0.join(name))?)
        }
    }
    verify_reader(
        &DirectoryView(assets, names.iter().cloned().collect()),
        Some(&root.join("config")),
        names,
        phase,
    )
}

/// Validates audio documents and media directly from the finished asset archive.
pub fn verify_archive(reader: &dyn super::publish::ArchiveReader, phase: &Phase) -> CoreResult<()> {
    let names = reader.names().into_iter().collect();
    verify_reader(reader, None, &names, phase)
}

fn verify_reader(
    reader: &dyn super::publish::ArchiveReader,
    gameplay_root: Option<&Path>,
    names: &BTreeSet<String>,
    phase: &Phase,
) -> CoreResult<()> {
    let mut inventory =
        json!({"banks":{},"sequences":{},"streams":{},"movies":{},"configs":{},"volumes":{}});
    fn document_category(
        reader: &dyn super::publish::ArchiveReader,
        gameplay_root: Option<&Path>,
        name: &str,
    ) -> CoreResult<Option<&'static str>> {
        if !name.ends_with(".json") {
            return Ok(None);
        }
        let bytes = asset(reader, gameplay_root, name)?;
        let name = name.strip_prefix("gameplay/").unwrap_or(name);
        let document = match serde_json::from_slice::<Value>(&bytes) {
            Ok(document) => document,
            Err(error) => {
                if name.ends_with(".sound.json")
                    || name.contains(".sound/")
                    || matches!(name, "snd2/rev.json" | "snd2/map_index.json")
                {
                    return Err(error.into());
                }
                return Ok(None);
            }
        };
        if name == "snd2/rev.json" || name == "snd2/map_index.json" {
            verify_scene_table(
                &document,
                if name == "snd2/rev.json" {
                    "scene_reverb"
                } else {
                    "scene_sound_routes"
                },
            )?;
            return Ok(None);
        }
        let expected_kind = if name.ends_with(".sound.json") {
            Some("sound_bundle")
        } else if name.contains(".sound/") {
            if name.ends_with("/bank.json") {
                Some("bank")
            } else if name.ends_with("/config.json") {
                Some("sound_config")
            } else if name.ends_with("/volume.json") {
                Some("sound_volume")
            } else if name.contains("/sequences/") {
                Some("sequence")
            } else if name.contains("/effects/") {
                Some("effect_sequence")
            } else {
                None
            }
        } else {
            None
        };
        if expected_kind.is_some_and(|kind| document["kind"] != kind) {
            return Err(fail(format!("invalid native audio document kind: {name}")));
        }
        let category = match document["kind"].as_str().unwrap_or("") {
            "bank" => "banks",
            "sequence" | "effect_sequence" => "sequences",
            "sound_config" => "configs",
            "sound_volume" => "volumes",
            "sound_bundle" => {
                if document["schema_version"] != 1 {
                    return Err(fail("invalid sound bundle version"));
                }
                for field in ["bank", "config", "volume"] {
                    if field == "bank" && document[field].is_null() {
                        continue;
                    }
                    let uri = document[field]
                        .as_str()
                        .ok_or_else(|| fail("sound bundle reference missing"))?;
                    let dependency: Value =
                        serde_json::from_slice(&asset(reader, gameplay_root, uri)?)?;
                    let kind = match field {
                        "bank" => "bank",
                        "config" => "sound_config",
                        _ => "sound_volume",
                    };
                    if dependency["schema_version"] != 1 || dependency["kind"] != kind {
                        return Err(fail("invalid sound bundle dependency"));
                    }
                }
                for (field, kind) in [
                    ("sequences", "sequence"),
                    ("effect_sequences", "effect_sequence"),
                ] {
                    for uri in document[field]
                        .as_array()
                        .ok_or_else(|| fail("sound bundle sequence list missing"))?
                    {
                        let dependency: Value = serde_json::from_slice(&asset(
                            reader,
                            gameplay_root,
                            uri.as_str().ok_or_else(|| fail("invalid sequence URI"))?,
                        )?)?;
                        if dependency["schema_version"] != 1 || dependency["kind"] != kind {
                            return Err(fail("invalid sound sequence dependency"));
                        }
                    }
                }
                return Ok(None);
            }
            _ => return Ok(None),
        };
        Ok(Some(category))
    }
    fn scan_file(
        reader: &dyn super::publish::ArchiveReader,
        gameplay_root: Option<&Path>,
        name: &str,
    ) -> CoreResult<(String, Option<&'static str>)> {
        if matches!(
            name.to_ascii_lowercase().as_str(),
            "snd2/rev.bin" | "snd2/map_index.txt"
        ) {
            return Err(fail(format!(
                "proprietary scene audio table published: {name}"
            )));
        }
        if name == "audio_catalog.json" {
            return Err(fail("obsolete audio catalog published"));
        }
        let category = document_category(reader, gameplay_root, name)?;
        Ok((name.to_owned(), category))
    }
    let scanned: Vec<CoreResult<(String, Option<&'static str>)>> = names
        .par_iter()
        .map(|name| {
            let result = scan_file(reader, gameplay_root, name);
            phase.inc(1);
            result
        })
        .collect();
    for entry in scanned {
        let (name, category) = entry?;
        if name.ends_with(".wav") {
            inventory["streams"][&name] = json!(name);
        }
        if name.ends_with(".mp4") {
            inventory["movies"][&name] = json!({"mp4":name});
        }
        if let Some(category) = category {
            inventory[category][&name] = json!(name);
        }
    }
    if let Some(gameplay_root) = gameplay_root {
        for (name, kind) in [
            ("snd2/rev.json", "scene_reverb"),
            ("snd2/map_index.json", "scene_sound_routes"),
        ] {
            if gameplay_root.join(name).is_file() {
                let document: Value =
                    serde_json::from_slice(&asset(reader, Some(gameplay_root), name)?)?;
                verify_scene_table(&document, kind)?;
            }
        }
        if gameplay_root.is_dir() {
            let mut loose = Vec::new();
            files_under(gameplay_root, &mut loose)?;
            for file in loose {
                let name = file
                    .strip_prefix(gameplay_root)
                    .unwrap()
                    .to_string_lossy()
                    .replace('\\', "/");
                let category = if name.ends_with(".sound/config.json") {
                    Some("configs")
                } else if name.ends_with(".sound/volume.json") {
                    Some("volumes")
                } else {
                    None
                };
                if let Some(category) = category {
                    if document_category(reader, Some(gameplay_root), &name)? != Some(category) {
                        return Err(fail(format!(
                            "invalid loose sound gameplay document: {name}"
                        )));
                    }
                    inventory[category][&name] = json!(name);
                }
            }
        }
    }
    if gameplay_root.is_some_and(|root| {
        root.join("snd2/rev.json").exists() != root.join("snd2/map_index.json").exists()
    }) {
        return Err(fail("scene audio tables must be published together"));
    }
    fn asset(
        reader: &dyn super::publish::ArchiveReader,
        gameplay_root: Option<&Path>,
        name: &str,
    ) -> CoreResult<Vec<u8>> {
        let p = Path::new(name);
        if p.is_absolute()
            || name.contains(['\\', ':', '\0'])
            || p.components()
                .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            return Err(fail("invalid audio asset path"));
        }
        let gameplay = format!("gameplay/{name}");
        if reader.contains(&gameplay) {
            reader.read(&gameplay)
        } else if reader.contains(name) {
            reader.read(name)
        } else if let Some(root) = gameplay_root.filter(|_| {
            name.ends_with(".sound/config.json")
                || name.ends_with(".sound/volume.json")
                || matches!(name, "snd2/rev.json" | "snd2/map_index.json")
        }) {
            Ok(fs::read(root.join(p))?)
        } else {
            Err(fail(format!("audio archive dependency missing: {name}")))
        }
    }
    fn check_wave(
        reader: &dyn super::publish::ArchiveReader,
        gameplay_root: Option<&Path>,
        name: &str,
    ) -> CoreResult<usize> {
        if !name.ends_with(".wav") {
            return Err(fail("audio sample is not WAV"));
        }
        let b = asset(reader, gameplay_root, name)?;
        if b.get(..4) != Some(b"RIFF")
            || b.get(8..16) != Some(b"WAVEfmt ")
            || u32le(&b, 16)? != 16
            || u16le(&b, 20)? != 1
            || u16le(&b, 34)? != 16
            || b.get(36..40) != Some(b"data")
            || u32le(&b, 40)? as usize + 44 != b.len()
        {
            return Err(fail(format!("invalid canonical PCM WAV: {name}")));
        }
        let channels = u16le(&b, 22)? as usize;
        if !(1..=2).contains(&channels) || u32le(&b, 24)? == 0 {
            return Err(fail("invalid WAV sample format"));
        }
        Ok((b.len() - 44) / (2 * channels))
    }
    fn check_resource(
        reader: &dyn super::publish::ArchiveReader,
        gameplay_root: Option<&Path>,
        category: &str,
        name: &str,
    ) -> CoreResult<()> {
        if category == "movies" {
            let b = asset(reader, gameplay_root, name)?;
            if !name.ends_with(".mp4") || b.get(4..8) != Some(b"ftyp") {
                return Err(fail("invalid movie MP4 asset"));
            }
            let mut at = 0;
            let mut boxes = BTreeSet::new();
            while at + 8 <= b.len() {
                let size = u32::from_be_bytes(b[at..at + 4].try_into().unwrap()) as usize;
                let size = if size == 1 {
                    u64::from_be_bytes(
                        b.get(at + 8..at + 16)
                            .ok_or_else(|| fail("truncated MP4 large box"))?
                            .try_into()
                            .unwrap(),
                    ) as usize
                } else if size == 0 {
                    b.len() - at
                } else {
                    size
                };
                if size < 8 || size > b.len() - at {
                    return Err(fail("invalid MP4 box size"));
                }
                boxes.insert(b[at + 4..at + 8].to_vec());
                at += size;
            }
            if at != b.len()
                || !boxes.contains(b"moov".as_slice())
                || !boxes.contains(b"mdat".as_slice())
            {
                return Err(fail("incomplete MP4 movie"));
            }
            return Ok(());
        }
        if category == "streams" {
            check_wave(reader, gameplay_root, name)?;
            return Ok(());
        }
        let document: Value = serde_json::from_slice(&asset(reader, gameplay_root, name)?)?;
        if !name.ends_with(".json") || document["schema_version"] != 1 {
            return Err(fail("unsupported audio document version"));
        }
        if category == "banks" {
            if document["kind"] != "bank"
                || !document["programs"].is_array()
                || !document["sample_sets"].is_array()
                || !document["samples"].is_array()
            {
                return Err(fail("invalid bank document"));
            }
            for sample in document["vags"]
                .as_array()
                .ok_or_else(|| fail("bank sample table missing"))?
            {
                if sample.is_null() {
                    continue;
                }
                let frames = check_wave(
                    reader,
                    gameplay_root,
                    sample["wav"]
                        .as_str()
                        .ok_or_else(|| fail("bank WAV missing"))?,
                )?;
                if sample["loop_start"]
                    .as_u64()
                    .is_none_or(|n| n >= frames as u64)
                    || sample["loop_end"] != frames as u64
                    || !sample["loops"].is_boolean()
                {
                    return Err(fail("invalid sample loop boundary"));
                }
            }
        } else if category == "configs" || category == "volumes" {
            if document["kind"]
                != if category == "configs" {
                    "sound_config"
                } else {
                    "sound_volume"
                }
                || !document["entries"].is_array()
                || !document["reverb"].is_array()
                || (category == "configs" && !document["entry_count"].is_u64())
            {
                return Err(fail("invalid sound manager metadata"));
            }
        } else if document["kind"] == "sequence" {
            for song in document["songs"]
                .as_array()
                .ok_or_else(|| fail("sequence songs missing"))?
            {
                if song["division"].as_u64().is_none_or(|n| n == 0) || !song["events"].is_array() {
                    return Err(fail("invalid sequence timing"));
                }
            }
        } else if document["kind"] == "effect_sequence" {
            if !document["events"].is_array()
                || document["tick_rate"].as_u64().is_none_or(|n| n == 0)
            {
                return Err(fail("invalid effect sequence"));
            }
        } else {
            return Err(fail("unknown audio sequence kind"));
        }
        Ok(())
    }
    fn resource<'a>(
        category: &str,
        alias: &str,
        value: &'a Value,
        checked: &mut BTreeSet<String>,
    ) -> CoreResult<Option<&'a str>> {
        if alias.is_empty() {
            return Err(fail("empty audio alias"));
        }
        let name = if category == "movies" {
            value["mp4"].as_str()
        } else {
            value.as_str()
        }
        .ok_or_else(|| fail("invalid audio resource value"))?;
        Ok(checked.insert(name.to_owned()).then_some(name))
    }
    let mut checked = BTreeSet::new();
    let mut items = Vec::new();
    for category in [
        "banks",
        "sequences",
        "streams",
        "movies",
        "configs",
        "volumes",
    ] {
        let Some(entries) = inventory[category].as_object() else {
            items.push(Err(fail("audio inventory category missing")));
            continue;
        };
        for (alias, value) in entries {
            match resource(category, alias, value, &mut checked) {
                Ok(Some(name)) => items.push(Ok((category, name))),
                Ok(None) => {}
                Err(error) => items.push(Err(error)),
            }
        }
    }
    let checks: Vec<CoreResult<()>> = items
        .par_iter()
        .map(|item| {
            let result = match item {
                Ok((category, name)) => check_resource(reader, gameplay_root, category, name),
                Err(_) => Ok(()),
            };
            phase.inc(1);
            result
        })
        .collect();
    let checked_items = items.len() as u64;
    for (item, check) in items.into_iter().zip(checks) {
        item?;
        check?;
    }
    let candidates = verify_work(names) - names.len() as u64 * 2;
    phase.inc(candidates.saturating_sub(checked_items));
    for name in names {
        let lower = name.to_ascii_lowercase();
        let basename = lower.rsplit('/').next().unwrap_or("");
        if ["snd", "hd", "bd", "sq", "mid", "pss"]
            .contains(&basename.rsplit('.').next().unwrap_or(""))
            || matches!(basename, "sound.dat" | "sound.hd3")
            || (lower.starts_with("snd2/") && matches!(basename, "rev.bin" | "map_index.txt"))
        {
            return Err(fail(format!("retail audio remains: {name}")));
        }
        phase.inc(1);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[ignore = "requires KZ_AUDIO_RAW and an isolated KZ_AUDIO_META_OUTPUT"]
    fn retail_sound_metadata_audit() {
        let raw = std::path::PathBuf::from(std::env::var_os("KZ_AUDIO_RAW").expect("KZ_AUDIO_RAW"));
        let output = std::path::PathBuf::from(
            std::env::var_os("KZ_AUDIO_META_OUTPUT").expect("KZ_AUDIO_META_OUTPUT"),
        );
        let files = collect(&raw, &Phase::noop()).unwrap();
        let mut configs = BTreeMap::new();
        let mut volumes = BTreeMap::new();
        let mut records = 0;
        for (key, b) in files {
            if key.ends_with(".cfg") || key.ends_with(".vol") {
                let volume = key.ends_with(".vol");
                let path = sound_metadata(&output, &key, &b, volume).unwrap();
                let doc: Value =
                    serde_json::from_slice(&fs::read(output.join(&path)).unwrap()).unwrap();
                records += doc["entries"].as_array().unwrap().len()
                    + doc["reverb"].as_array().unwrap().len();
                if volume {
                    volumes.insert(key, path);
                } else {
                    configs.insert(key, path);
                }
            }
        }
        eprintln!(
            "sound metadata: {} configs, {} volumes, {records} semantic records",
            configs.len(),
            volumes.len()
        );
        write_json(
            &output,
            "sound_metadata_catalog.json",
            &json!({"schema_version":1,"configs":configs,"volumes":volumes}),
        )
        .unwrap();
    }
    #[test]
    fn scene_audio_tables_publish_53_minimal_records_and_preserve_routing_columns() {
        let temporary = tempfile::tempdir().unwrap();
        let raw = temporary.path().join("raw");
        let output = temporary.path().join("published");
        fs::create_dir_all(raw.join("snd2")).unwrap();
        fs::create_dir_all(output.join("data/snd2")).unwrap();
        let mut reverb = Vec::new();
        for id in 0..53i16 {
            for value in [id, 4, id + 1, 1234] {
                reverb.extend(value.to_le_bytes());
            }
        }
        assert_eq!(reverb.len(), 424);
        fs::write(raw.join("snd2/rev.bin"), &reverb).unwrap();
        fs::write(output.join("data/snd2/rev.bin"), &reverb).unwrap();
        let first: [&[u8]; 22] = [
            b"1000",
            b"label",
            b"BS",
            b"description with spaces",
            b"BG_002",
            b"BS_003",
            b"FG_004",
            b"SR_005",
            b"\x8e\x9e\x8a\xd4\x95\xcf\x89\xbb*0.1",
            b"OB_007",
            b"OB_009",
            b"-",
            b"-",
            b"-",
            b"-",
            b"EV_001_002",
            b"-",
            b"-",
            b"-",
            b"SY_000",
            b"-",
            b"RV_003",
        ];
        let mut second = first;
        second[0] = b"1001";
        second[8] = b"SR_005_006";
        second[9] = b"*";
        second[10] = b"EV_004_005";
        second[16] = b"RV_019";
        let routes = [first.join(&b'\t'), second.join(&b'\t')].join(&b'\n');
        fs::write(raw.join("snd2/map_index.txt"), &routes).unwrap();
        fs::write(output.join("data/snd2/map_index.txt"), &routes).unwrap();
        convert_tree(&raw, &output, 1).unwrap();
        verify(&output).unwrap();
        let reverbs: Value =
            serde_json::from_slice(&fs::read(output.join("data/snd2/rev.json")).unwrap()).unwrap();
        let routes: Value =
            serde_json::from_slice(&fs::read(output.join("data/snd2/map_index.json")).unwrap())
                .unwrap();
        assert_eq!(reverbs["rows"].as_array().unwrap().len(), 53);
        assert_eq!(reverbs["rows"][52], json!({"id":52,"type":4,"depth":53}));
        assert_eq!(routes["rows"][0]["env_vol"], 12);
        assert_eq!(
            routes["rows"][0]["se_src"],
            json!([7, 9, -1, -1, -1, -1, -1, -1])
        );
        assert_eq!(routes["rows"][0]["event_se"], json!([1, 2]));
        assert_eq!(routes["rows"][0]["reverb_id"], 3);
        assert_eq!(routes["rows"][1]["env_bgm"], 6);
        assert_eq!(
            routes["rows"][1]["se_src"],
            json!([-2, -1, -1, -1, -1, -1, -1, -1])
        );
        assert_eq!(routes["rows"][1]["event_se"], json!([4, 5]));
        assert_eq!(routes["rows"][1]["reverb_id"], 19);
        assert!(!output.join("data/snd2/rev.bin").exists());
        assert!(!output.join("data/snd2/map_index.txt").exists());
        assert!(scene_reverb(&reverb[..423]).is_err());
        fs::write(output.join("data/snd2/rev.json"), b"invalid JSON").unwrap();
        assert!(verify(&output).is_err());
    }
    #[test]
    #[ignore = "requires KZ_AUDIO_RAW and isolated KZ_AUDIO_SCENE_OUTPUT"]
    fn retail_scene_audio_table_audit() {
        let raw = std::path::PathBuf::from(std::env::var_os("KZ_AUDIO_RAW").unwrap());
        let output = std::path::PathBuf::from(std::env::var_os("KZ_AUDIO_SCENE_OUTPUT").unwrap());
        let reverb = scene_reverb(&fs::read(raw.join("snd2/rev.bin")).unwrap()).unwrap();
        let routes =
            scene_sound_routes(&fs::read(raw.join("snd2/map_index.txt")).unwrap()).unwrap();
        verify_scene_table(&reverb, "scene_reverb").unwrap();
        verify_scene_table(&routes, "scene_sound_routes").unwrap();
        assert_eq!(reverb["rows"].as_array().unwrap().len(), 53);
        write_json(&output, "snd2/rev.json", &reverb).unwrap();
        write_json(&output, "snd2/map_index.json", &routes).unwrap();
        eprintln!(
            "scene audio: {} reverb rows, {} map routing rows",
            reverb["rows"].as_array().unwrap().len(),
            routes["rows"].as_array().unwrap().len()
        );
    }
    #[test]
    fn sound_config_columns_and_volume_rows_preserve_effect_mapping() {
        let temp = tempfile::tempdir().unwrap();
        let text=b"REVERBE\tHall\t64\r\n0\tEffect\tDescription\tFX\tKeyOn\t4\t60\t*\r\n1\tSeq\tDescription\tFX\tloop.mid\t0\t0\t\r\nEND\r\n";
        let path = sound_metadata(temp.path(), "test.cfg", text, false).unwrap();
        let doc: Value =
            serde_json::from_slice(&fs::read(temp.path().join(path)).unwrap()).unwrap();
        assert_eq!(doc["entry_count"], 3);
        assert_eq!(doc["entries"][0]["filename"], "KeyOn");
        assert_eq!(doc["entries"][0]["program"], 4);
        assert_eq!(doc["entries"][0]["flag"], true);
        assert_eq!(doc["entries"][1]["flag"], false);
        assert_eq!(doc["reverb"][0]["type"], 5);
        let path = sound_metadata(
            temp.path(),
            "test.vol",
            b"REVERB\t5\t40\r\n0\t127\r\n1\t91\r\nEND\r\n",
            true,
        )
        .unwrap();
        let doc: Value =
            serde_json::from_slice(&fs::read(temp.path().join(path)).unwrap()).unwrap();
        assert_eq!(doc["entries"][1]["index"], 1);
        assert_eq!(doc["entries"][1]["volume"], 91);
        assert_eq!(doc["reverb"][0]["depth"], 40);
    }
    fn table(name: &[u8; 4], records: &[Vec<u8>]) -> Vec<u8> {
        let mut b = Vec::from(&b"SCEI"[..]);
        b.extend(name);
        b.extend(0u32.to_le_bytes());
        b.extend((records.len() as u32 - 1).to_le_bytes());
        let mut offset = 16 + 4 * records.len();
        for r in records {
            b.extend((offset as u32).to_le_bytes());
            offset += r.len();
        }
        for r in records {
            b.extend(r);
        }
        offset = (offset + 3) & !3;
        b.resize(offset, 0);
        b[8..12].copy_from_slice(&(offset as u32).to_le_bytes());
        b
    }
    #[test]
    fn bank_conversion_preserves_split_envelope_and_removes_retail_payload() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let stage = temp.path().join("stage");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(stage.join("data")).unwrap();
        let mut program = vec![0; 56];
        program[0..4].copy_from_slice(&36u32.to_le_bytes());
        program[4] = 1;
        program[5] = 20;
        program[6] = 127;
        program[7] = 64;
        program[40] = 127;
        program[42..44].copy_from_slice(&256u16.to_le_bytes());
        program[44..46].copy_from_slice(&384u16.to_le_bytes());
        program[52] = 127;
        program[53] = 64;
        let mut sample = vec![0; 42];
        sample[4] = 127;
        sample[11] = 60;
        sample[13] = 64;
        sample[16] = 127;
        sample[18..20].copy_from_slice(&0x80ffu16.to_le_bytes());
        sample[20..22].copy_from_slice(&0x5fc0u16.to_le_bytes());
        sample[41] = 15;
        let mut info = vec![0; 8];
        info[4..6].copy_from_slice(&22050u16.to_le_bytes());
        let hd = [
            table(b"Prog", &[program]),
            table(b"Sset", &[vec![0, 0, 127, 1, 0, 0]]),
            table(b"Smpl", &[sample]),
            table(b"Vagi", &[info]),
        ]
        .concat();
        let mut bd = vec![0; 16];
        bd[1] = 7;
        bd[2] = 0x21;
        fs::write(raw.join("test.hd"), &hd).unwrap();
        fs::write(raw.join("test.bd"), &bd).unwrap();
        fs::write(stage.join("data/test.hd"), &hd).unwrap();
        fs::write(stage.join("data/test.bd"), &bd).unwrap();
        convert_tree(&raw, &stage, 1).unwrap();
        verify(&stage).unwrap();
        assert!(!stage.join("data/test.hd").exists());
        assert!(!stage.join("data/test.bd").exists());
        let d: Value =
            serde_json::from_slice(&fs::read(stage.join("data/test.sound/bank.json")).unwrap())
                .unwrap();
        assert_eq!(d["programs"][0]["splits"][0]["bend_high"], 384);
        assert_eq!(d["samples"][0]["adsr1"], 0x80ff);
        assert_eq!(d["vags"][0]["loop_end"], 28);
        assert_eq!(d["vags"][0]["loops"], true);
        fs::write(stage.join("data/test.sound/samples/0.wav"), b"bad").unwrap();
        assert!(verify(&stage).is_err());
    }

    #[test]
    fn effect_midi_normalizes_running_status_and_retail_clock() {
        let temp = tempfile::tempdir().unwrap();
        let track = [
            0, 0x90, 60, 100, 10, 61, 90, 0, 0x80, 60, 0, 0, 0xff, 0x2f, 0,
        ];
        let mut b = Vec::from(&b"MThd\0\0\0\x06\0\0\0\x01\x01\xe0MTrk"[..]);
        b.extend((track.len() as u32).to_be_bytes());
        b.extend(track);
        let name = smf(temp.path(), "test.mid", &b).unwrap();
        let d: Value = serde_json::from_slice(&fs::read(temp.path().join(name)).unwrap()).unwrap();
        assert_eq!(d["tick_rate"], 1800);
        assert_eq!(d["events"].as_array().unwrap().len(), 3);
        assert_eq!(d["events"][1]["status"], 144);
        assert_eq!(d["events"][1]["delta"], 10);
    }

    #[test]
    fn archived_gameplay_scene_tables_validate_their_kind() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("data");
        fs::create_dir_all(root.join("gameplay/snd2")).unwrap();
        let table = root.join("gameplay/snd2/rev.json");
        let archive_path = temp.path().join("data.zip");
        for kind in ["scene_reverb", "wrong_kind"] {
            fs::write(
                &table,
                serde_json::to_vec(&json!({"schema_version":1,"kind":kind,"rows":[]})).unwrap(),
            )
            .unwrap();
            super::super::zip_archive::pack_directory(&root, &archive_path).unwrap();
            let archive = super::super::zip_archive::Archive::open(&archive_path).unwrap();
            let result = verify_archive(
                &super::super::publish::ArchiveView::new(&archive),
                &Phase::noop(),
            );
            assert_eq!(result.is_ok(), kind == "scene_reverb");
        }
    }

    #[test]
    fn direct_bundle_preserves_member_order_and_resource_references() {
        let temp = tempfile::tempdir().unwrap();
        let raw = temp.path().join("raw");
        let stage = temp.path().join("stage");
        fs::create_dir_all(&raw).unwrap();
        fs::create_dir_all(stage.join("data")).unwrap();
        let track = [0, 0x90, 60, 100, 0, 0xff, 0x2f, 0];
        let mut midi = Vec::from(&b"MThd"[..]);
        midi.extend(6u32.to_be_bytes());
        midi.extend([0, 0, 0, 1, 1, 0xe0]);
        midi.extend(b"MTrk");
        midi.extend((track.len() as u32).to_be_bytes());
        midi.extend(track);
        let names = ["z.mid", "a.mid", "test.cfg", "test.vol"];
        let payloads = [
            midi.clone(),
            midi,
            b"0\tEffect\tDescription\tFX\tz.mid\t0\t60\t\r\nEND\r\n".to_vec(),
            b"0\t91\r\nEND\r\n".to_vec(),
        ];
        let mut bytes = vec![0; 76 * (names.len() + 1)];
        for i in 0..names.len() {
            let at = i * 76;
            let offset = bytes.len() - at;
            bytes[at..at + names[i].len()].copy_from_slice(names[i].as_bytes());
            bytes[at + 64..at + 68].copy_from_slice(&(offset as u32).to_le_bytes());
            bytes[at + 68..at + 72].copy_from_slice(&(payloads[i].len() as u32).to_le_bytes());
            bytes[at + 72..at + 76].copy_from_slice(&76u32.to_le_bytes());
            bytes.extend(&payloads[i]);
        }
        fs::write(raw.join("test.snd"), &bytes).unwrap();
        fs::write(stage.join("data/test.snd"), bytes).unwrap();
        convert_tree(&raw, &stage, 1).unwrap();
        verify(&stage).unwrap();
        let bundle: Value =
            serde_json::from_slice(&fs::read(stage.join("data/test.sound.json")).unwrap()).unwrap();
        assert_eq!(
            bundle["effect_sequences"],
            json!(["test.sound/effects/z.json", "test.sound/effects/a.json"])
        );
        let config: Value =
            serde_json::from_slice(&fs::read(stage.join("data/test.sound/config.json")).unwrap())
                .unwrap();
        assert_eq!(
            config["entries"][0]["filename"],
            "test.sound/effects/z.json"
        );
        assert!(!stage.join("data/test.snd").exists());
        assert!(!stage.join("data/audio_catalog.json").exists());
        fs::create_dir_all(stage.join("data/gameplay/test.sound")).unwrap();
        for name in ["config.json", "volume.json"] {
            fs::rename(
                stage.join("data/test.sound").join(name),
                stage.join("data/gameplay/test.sound").join(name),
            )
            .unwrap();
        }
        let archive_path = temp.path().join("data.zip");
        super::super::zip_archive::pack_directory(&stage.join("data"), &archive_path).unwrap();
        let archive = super::super::zip_archive::Archive::open(&archive_path).unwrap();
        verify_archive(
            &super::super::publish::ArchiveView::new(&archive),
            &Phase::noop(),
        )
        .unwrap();
        fs::remove_file(stage.join("data/test.sound/effects/z.json")).unwrap();
        assert!(verify(&stage).is_err());
    }
    #[test]
    #[ignore = "requires KZ_AUDIO_RAW and isolated KZ_AUDIO_DIRECT_OUTPUT"]
    fn retail_direct_audio_audit() {
        let raw = std::path::PathBuf::from(std::env::var_os("KZ_AUDIO_RAW").expect("KZ_AUDIO_RAW"));
        let output = std::path::PathBuf::from(
            std::env::var_os("KZ_AUDIO_DIRECT_OUTPUT").expect("KZ_AUDIO_DIRECT_OUTPUT"),
        );
        convert_tree(&raw, &output, 1).unwrap();
        verify(&output).unwrap();
    }

    #[test]
    fn pack_stripping_retains_neighbor_text_and_removes_audio_bytes() {
        let names = ["one.wav", "settings.cfg"];
        let payloads = [wav(&[1, 2, 3], 48000, 1).unwrap(), b"volume=127".to_vec()];
        let mut pack = vec![0; 76 * 3];
        for i in 0..2 {
            let at = i * 76;
            pack[at..at + names[i].len()].copy_from_slice(names[i].as_bytes());
            let offset = pack.len() - at;
            pack[at + 64..at + 68].copy_from_slice(&(offset as u32).to_le_bytes());
            pack[at + 68..at + 72].copy_from_slice(&(payloads[i].len() as u32).to_le_bytes());
            pack[at + 72..at + 76].copy_from_slice(&76u32.to_le_bytes());
            pack.extend(&payloads[i]);
        }
        let mut clean = pack.clone();
        strip_pack(&pack, &mut clean, 0).unwrap();
        assert_eq!(u32le(&clean, 68).unwrap(), 0);
        let offset = u32le(&pack, 64).unwrap() as usize;
        assert!(
            clean[offset..offset + payloads[0].len()]
                .iter()
                .all(|v| *v == 0)
        );
        let offset = 76 + u32le(&pack, 140).unwrap() as usize;
        assert_eq!(&clean[offset..offset + payloads[1].len()], b"volume=127");
    }
    #[test]
    fn spu_predictor_loop_and_wave() {
        let mut b = [0u8; 32];
        b[1] = 4;
        b[2] = 0x21;
        b[17] = 3;
        let (p, start, loops) = adpcm(&b).unwrap();
        assert_eq!(&p[..2], &[4096, 8192]);
        assert_eq!(p.len(), 56);
        assert_eq!(start, 0);
        assert!(loops);
        let w = wav(&p, 22050, 1).unwrap();
        assert_eq!(&w[..4], b"RIFF");
        assert_eq!(u32le(&w, 40).unwrap(), 112);
    }
    #[test]
    fn spu_prediction_carries_history_rounds_signed_values_and_clips() {
        let mut b = [0x11; 32];
        b[0] = 0x1c;
        b[1] = 4;
        b[16] = 0x2c;
        b[17] = 3;
        b[18..].fill(0xff);
        let (p, _, _) = adpcm(&b).unwrap();
        assert_eq!(&p[..10], &[1, 2, 3, 4, 5, 6, 7, 8, 9, 9]);
        assert_eq!(&p[28..36], &[8, 6, 3, 0, -3, -6, -9, -12]);
        b[..16].fill(0x77);
        b[0] = 0x40;
        b[1] = 1;
        let (p, _, _) = adpcm(&b).unwrap();
        assert_eq!(&p[..3], &[28672, 32767, 32767]);
    }
    #[test]
    fn sequence_running_status_no_delta_and_tempo() {
        let b = [
            0, 0x90, 60, 0xff, 61, 64, 10, 0x80, 60, 0, 0xff, 0x51, 3, 7, 0xa1, 0x20, 0, 0xff,
            0x2f, 0,
        ];
        let e = events(&b).unwrap();
        assert_eq!(e[1]["delta"], 0);
        assert_eq!(e[1]["data"][0], 61);
        assert_eq!(e[2]["delta"], 10);
        assert_eq!(e[3]["meta"], 81);
        assert_eq!(e[4]["kind"], "end");
    }
    #[test]
    fn planar_movie_pcm_and_truncation() {
        let mut b = Vec::from(&b"SShd"[..]);
        b.extend(24u32.to_le_bytes());
        for n in [1u32, 48000, 2, 4, 0, 0] {
            b.extend(n.to_le_bytes());
        }
        b.extend(b"SSbd");
        b.extend(8u32.to_le_bytes());
        for n in [1i16, 2, 3, 4] {
            b.extend(n.to_le_bytes());
        }
        let (p, r, c) = sshd(&b).unwrap();
        assert_eq!(p, [1, 3, 2, 4]);
        assert_eq!((r, c), (48000, 2));
        b.pop();
        assert!(sshd(&b).is_err());
    }
    #[test]
    fn movie_pts_aligns_audio_and_trims_only_frames_before_video() {
        fn pes(id: u8, pts: u64, payload: &[u8]) -> Vec<u8> {
            let timestamp = [
                0x21 | ((pts >> 29) as u8 & 14),
                (pts >> 22) as u8,
                ((pts >> 14) as u8 & 254) | 1,
                (pts >> 7) as u8,
                ((pts << 1) as u8) | 1,
            ];
            let mut out = vec![0, 0, 1, id];
            out.extend(((8 + payload.len()) as u16).to_be_bytes());
            out.extend([128, 128, 5]);
            out.extend(timestamp);
            out.extend(payload);
            out
        }
        let mut pcm = Vec::from(&b"SShd"[..]);
        pcm.extend(24u32.to_le_bytes());
        for value in [1u32, 48000, 1, 4, 0, 0] {
            pcm.extend(value.to_le_bytes());
        }
        pcm.extend(b"SSbd");
        pcm.extend(960u32.to_le_bytes());
        for value in 0..480i16 {
            pcm.extend(value.to_le_bytes());
        }
        let mut payload = vec![255, 160, 0, 0];
        payload.extend(pcm);
        let video = pes(224, 90000, &[0, 0, 1, 179]);
        let positive = [video.clone(), pes(189, 90450, &payload)].concat();
        let (wave, timing) = movie(&positive).unwrap().unwrap();
        assert_eq!(timing["offset_frames"], 240);
        assert_eq!(u32le(&wave, 40).unwrap(), 1440);
        assert!(wave[44..44 + 480].iter().all(|v| *v == 0));
        assert_eq!(u16le(&wave, 44 + 480 + 2).unwrap(), 1);
        let negative = [video, pes(189, 89550, &payload)].concat();
        let (wave, timing) = movie(&negative).unwrap().unwrap();
        assert_eq!(timing["offset_frames"], -240);
        assert_eq!(u32le(&wave, 40).unwrap(), 480);
        assert_eq!(u16le(&wave, 44).unwrap(), 240);
    }
    #[test]
    fn unknown_codec_fails() {
        assert!(stream(b"unknown").is_err());
    }
}
