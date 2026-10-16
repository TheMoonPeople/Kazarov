//! Converts retail sound resources to PCM WAV and semantic JSON before publication.
use super::{CoreError, CoreResult};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::Path;

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
    for block in b.chunks_exact(16) {
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
    if rate == 0 || !(1..=2).contains(&channels) || pcm.len() % channels as usize != 0 {
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

fn bank(root: &Path, key: &str, hd: &[u8], bd: &[u8]) -> CoreResult<String> {
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
        write(root, &sample_path, &wav(&pcm, rate as u32, 1)?)?;
        vags.push(json!({"wav":sample_path,"attr":p[6],"loop_start":loop_start,"loop_end":pcm.len(),"loops":loops}));
    }
    let document = path(key, "bank.json");
    write_json(
        root,
        &document,
        &json!({"schema_version":1,"kind":"bank","programs":programs,"sample_sets":sets,"samples":samples,"vags":vags}),
    )?;
    Ok(document)
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
fn sequence(root: &Path, key: &str, sq: &[u8]) -> CoreResult<String> {
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
    write_json(
        root,
        &document,
        &json!({"schema_version":1,"kind":"sequence","songs":songs}),
    )?;
    Ok(document)
}

fn smf(root: &Path, key: &str, b: &[u8]) -> CoreResult<String> {
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
    write_json(
        root,
        &document,
        &json!({"schema_version":1,"kind":"effect_sequence","tick_rate":division as u32*225/60,"events":out}),
    )?;
    Ok(document)
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
    if b.len() % 8 != 0 || b.len() / 8 > 256 {
        return Err(fail("invalid scene reverb table size"));
    }
    let mut rows = Vec::new();
    for record in b.chunks_exact(8) {
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
fn sound_metadata(root: &Path, key: &str, b: &[u8], volume: bool) -> CoreResult<String> {
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
    let document = path(key, if volume { "volume.json" } else { "config.json" });
    let kind = if volume {
        "sound_volume"
    } else {
        "sound_config"
    };
    let mut value = json!({"schema_version":1,"kind":kind,"entries":entries,"reverb":reverb});
    if !volume {
        value["entry_count"] = json!(rows.len());
    }
    write_json(root, &document, &value)?;
    Ok(document)
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
    if !(1..=2).contains(&channels) || interleave == 0 || interleave % 2 != 0 {
        return Err(fail("invalid SShd layout"));
    }
    let bytes = u32le(b, body + 4)? as usize;
    let data = b
        .get(body + 8..body + 8 + bytes)
        .ok_or_else(|| fail("truncated SShd body"))?;
    let mut pcm = Vec::with_capacity(bytes / 2);
    for group in data.chunks(interleave * channels) {
        let stride = group.len() / channels;
        if stride % 2 != 0 {
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
            &d.chunks_exact(2)
                .map(|s| i16::from_le_bytes([s[0], s[1]]))
                .collect::<Vec<_>>(),
            rate,
            channels,
        );
    }
    if codec != 0x69 || !(1..=2).contains(&channels) {
        return Err(fail(format!("unsupported WAV codec {codec:#x}")));
    }
    let block_align = u16le(f, 12)? as usize;
    if block_align < 16 * channels as usize || block_align % (16 * channels as usize) != 0 {
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

fn movie_mp4(root: &Path, key: &str, b: &[u8]) -> CoreResult<String> {
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};
    static SERIAL: AtomicU64 = AtomicU64::new(0);
    struct Temporary(std::path::PathBuf);
    impl Drop for Temporary {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    let temporary = Temporary(root.parent().unwrap_or(root).join(format!(
        ".movie-work-{}-{}",
        std::process::id(),
        SERIAL.fetch_add(1, Ordering::Relaxed)
    )));
    fs::create_dir_all(&temporary.0)?;
    let video = temporary.0.join("video.mpeg");
    fs::write(&video, strip_movie_audio(b)?)?;
    let audio = movie(b)?;
    let audio_path = temporary.0.join("audio.wav");
    if let Some((wav, _)) = &audio {
        fs::write(&audio_path, wav)?;
    }
    let name = key
        .strip_prefix("movie/")
        .unwrap_or(key)
        .trim_end_matches(".pss");
    let target = format!("movie/{name}.mp4");
    fs::create_dir_all(root.join(&target).parent().unwrap())?;
    let sibling = std::env::current_exe().ok().and_then(|p| {
        p.parent().map(|p| {
            p.join(if cfg!(windows) {
                "ffmpeg.exe"
            } else {
                "ffmpeg"
            })
        })
    });
    let executable = std::env::var_os("KAZAROV_FFMPEG")
        .map(std::path::PathBuf::from)
        .or_else(|| sibling.filter(|p| p.is_file()))
        .or_else(|| {
            option_env!("KAZAROV_FFMPEG")
                .map(std::path::PathBuf::from)
                .filter(|p| p.is_file())
        })
        .unwrap_or_else(|| "ffmpeg".into());
    let mut command = Command::new(&executable);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    command
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-y",
            "-copyts",
            "-start_at_zero",
            "-i",
        ])
        .arg(&video);
    if audio.is_some() {
        command.arg("-i").arg(&audio_path);
    }
    command.args(["-map", "0:v:0"]);
    if audio.is_some() {
        command.args(["-map", "1:a:0", "-c:a", "alac"]);
    }
    command
        .args([
            "-c:v",
            "copy",
            "-map_metadata",
            "-1",
            "-fflags",
            "+bitexact",
            "-flags:a",
            "+bitexact",
            "-threads",
            "1",
            "-movflags",
            "+faststart",
            "-f",
            "mp4",
        ])
        .arg(root.join(&target));
    let result = command.output().map_err(|e| {
        fail(format!(
            "{}: movie MP4 conversion requires FFmpeg: {e}",
            executable.display()
        ))
    })?;
    if !result.status.success() {
        return Err(fail(format!(
            "FFmpeg MP4 conversion failed: {}",
            String::from_utf8_lossy(&result.stderr)
        )));
    }
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
fn collect(root: &Path, dir: &Path, files: &mut BTreeMap<String, Vec<u8>>) -> CoreResult<()> {
    for e in fs::read_dir(dir)? {
        let p = e?.path();
        if p.is_dir() {
            collect(root, &p, files)?;
        } else {
            let key = p
                .strip_prefix(root)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/")
                .to_ascii_lowercase();
            if key == "sound/sound.dat" || key == "sound/sound.hd3" {
                continue;
            }
            if ["cfg", "vol"].contains(&p.extension().and_then(|s| s.to_str()).unwrap_or(""))
                && ["hd", "sq", "mid"]
                    .iter()
                    .any(|ext| p.with_extension(ext).is_file())
            {
                files.insert(key, fs::read(&p)?);
                continue;
            }
            let b = fs::read(&p)?;
            expand(&key, b, files, 0)?;
        }
    }
    Ok(())
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

fn read_source(raw: &Path, key: &str) -> CoreResult<Vec<u8>> {
    let components: Vec<_> = key.split('/').collect();
    let mut physical = raw.to_path_buf();
    for (i, component) in components.iter().enumerate() {
        physical.push(component);
        if !physical.is_file() {
            continue;
        }
        let mut bytes = fs::read(&physical)?;
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

/// Publishes directly addressed sound bundles, recordings, and self-contained movies.
pub fn convert_tree(raw: &Path, published: &Path, _jobs: usize) -> CoreResult<()> {
    let assets = published.join("port_data");
    let published_assets = assets.as_path();
    let mut files = BTreeMap::new();
    collect(raw, raw, &mut files)?;
    let (mut banks, mut sequences, mut streams, mut movies) = (
        BTreeMap::new(),
        BTreeMap::new(),
        BTreeMap::new(),
        BTreeMap::new(),
    );
    let (mut configs, mut volumes) = (BTreeMap::new(), BTreeMap::new());
    for (key, b) in &files {
        let result = (|| -> CoreResult<()> {
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
                banks.insert(key.clone(), document.clone());
                banks.insert(bd_key, document);
            } else if key.ends_with(".sq") {
                sequences.insert(
                    key.clone(),
                    Value::String(sequence(published_assets, key, b)?),
                );
            } else if key.ends_with(".mid") {
                sequences.insert(key.clone(), Value::String(smf(published_assets, key, b)?));
            } else if key.ends_with(".cfg") {
                configs.insert(
                    key.clone(),
                    Value::String(sound_metadata(published_assets, key, b, false)?),
                );
            } else if key.ends_with(".vol") {
                volumes.insert(
                    key.clone(),
                    Value::String(sound_metadata(published_assets, key, b, true)?),
                );
            } else if key.ends_with(".wav") || b.starts_with(b"SShd") || b.starts_with(b"RIFF") {
                let target = format!("{}.wav", key.trim_end_matches(".wav"));
                write(published_assets, &target, &stream(b)?)?;
                streams.insert(key.clone(), Value::String(target));
            } else if key.ends_with(".pss") {
                movies.insert(key.clone(), movie_mp4(published_assets, key, b)?);
            }
            Ok(())
        })();
        result.map_err(|e| fail(format!("{key}: {e}")))?;
    }
    let mut groups = BTreeSet::new();
    for key in files.keys() {
        if let Some((prefix, _)) = key.split_once(".snd/") {
            groups.insert(format!("{prefix}.snd"));
        }
    }
    for group in groups {
        let original = read_source(raw, &group)?;
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
        let config_path = config_ref.as_str().unwrap();
        let mut config: Value = serde_json::from_slice(&fs::read(assets.join(config_path))?)?;
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
        write_json(&assets, config_path, &config)?;
        write_json(
            &assets,
            &format!("{}.sound.json", group.trim_end_matches(".snd")),
            &json!({"schema_version":1,"kind":"sound_bundle","bank":bank_ref,"sequences":sequence_refs,"effect_sequences":effect_refs,"config":config_ref,"volume":volume_ref}),
        )?;
    }
    strip_tree(raw, raw, &published.join("data"))
}

fn strip_tree(raw: &Path, dir: &Path, output: &Path) -> CoreResult<()> {
    for entry in fs::read_dir(dir)? {
        let p = entry?.path();
        if p.is_dir() {
            strip_tree(raw, &p, output)?;
            continue;
        }
        let relative = p.strip_prefix(raw).unwrap();
        let key = relative
            .to_string_lossy()
            .replace('\\', "/")
            .to_ascii_lowercase();
        let destination = output.join(relative);
        if matches!(
            key.as_str(),
            "sound/sound.dat" | "sound/sound.hd3" | "snd2/rev.bin" | "snd2/map_index.txt"
        ) {
            if destination.exists() {
                fs::remove_file(destination)?;
            }
            continue;
        }
        if ["snd", "hd", "bd", "sq", "mid", "wav", "pss"]
            .contains(&key.rsplit('.').next().unwrap_or(""))
        {
            if destination.exists() {
                fs::remove_file(destination)?;
            }
            continue;
        }
        if ["cfg", "vol"].contains(&p.extension().and_then(|s| s.to_str()).unwrap_or(""))
            && ["hd", "sq", "mid"]
                .iter()
                .any(|ext| p.with_extension(ext).is_file())
        {
            if destination.exists() {
                fs::remove_file(destination)?;
            }
            continue;
        }
        // Model and texture passes may already have removed a whole converted source.
        if !destination.is_file() {
            continue;
        }
        let original = fs::read(&p)?;
        if original.starts_with(b"SShd")
            || (original.starts_with(b"RIFF") && original.get(8..12) == Some(b"WAVE"))
        {
            fs::remove_file(destination)?;
            continue;
        }
        if pack(&original).is_some() {
            let mut current = fs::read(&destination)?;
            strip_pack(&original, &mut current, 0)?;
            fs::write(destination, current)?;
        }
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
        } else if pack(b).is_some() {
            if u32le(current, cursor + 68)? as usize == b.len() {
                strip_pack(
                    b,
                    &mut current[cursor + offset..cursor + offset + b.len()],
                    depth + 1,
                )?;
            }
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
    let assets = root.join("port_data");
    if !assets.is_dir() {
        return Ok(());
    }
    let mut inventory =
        json!({"banks":{},"sequences":{},"streams":{},"movies":{},"configs":{},"volumes":{}});
    fn inventory_tree(root: &Path, dir: &Path, inventory: &mut Value) -> CoreResult<()> {
        for entry in fs::read_dir(dir)? {
            let p = entry?.path();
            if p.is_dir() {
                inventory_tree(root, &p, inventory)?;
                continue;
            }
            let name = p
                .strip_prefix(root)
                .unwrap()
                .to_string_lossy()
                .replace('\\', "/");
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
            if name.ends_with(".wav") {
                inventory["streams"][&name] = json!(name);
            }
            if name.ends_with(".mp4") {
                inventory["movies"][&name] = json!({"mp4":name});
            }
            if !name.ends_with(".json") {
                continue;
            }
            let bytes = fs::read(&p)?;
            let document = match serde_json::from_slice::<Value>(&bytes) {
                Ok(document) => document,
                Err(error) => {
                    if name.ends_with(".sound.json")
                        || name.contains(".sound/")
                        || matches!(name.as_str(), "snd2/rev.json" | "snd2/map_index.json")
                    {
                        return Err(error.into());
                    }
                    continue;
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
                continue;
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
                        let dependency: Value = serde_json::from_slice(&asset(root, uri)?)?;
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
                                root,
                                uri.as_str().ok_or_else(|| fail("invalid sequence URI"))?,
                            )?)?;
                            if dependency["schema_version"] != 1 || dependency["kind"] != kind {
                                return Err(fail("invalid sound sequence dependency"));
                            }
                        }
                    }
                    continue;
                }
                _ => continue,
            };
            inventory[category][&name] = json!(name);
        }
        Ok(())
    }
    inventory_tree(&assets, &assets, &mut inventory)?;
    if assets.join("snd2/rev.json").exists() != assets.join("snd2/map_index.json").exists() {
        return Err(fail("scene audio tables must be published together"));
    }
    fn asset(root: &Path, name: &str) -> CoreResult<Vec<u8>> {
        let p = Path::new(name);
        if p.is_absolute()
            || name.contains(['\\', ':', '\0'])
            || p.components()
                .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            return Err(fail("invalid audio asset path"));
        }
        Ok(fs::read(root.join(p))?)
    }
    fn check_wave(root: &Path, name: &str) -> CoreResult<usize> {
        if !name.ends_with(".wav") {
            return Err(fail("audio sample is not WAV"));
        }
        let b = asset(root, name)?;
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
    let mut checked = BTreeSet::new();
    for category in [
        "banks",
        "sequences",
        "streams",
        "movies",
        "configs",
        "volumes",
    ] {
        let entries = inventory[category]
            .as_object()
            .ok_or_else(|| fail("audio inventory category missing"))?;
        for (alias, value) in entries {
            if alias.is_empty() {
                return Err(fail("empty audio alias"));
            }
            let name = if category == "movies" {
                value["mp4"].as_str()
            } else {
                value.as_str()
            }
            .ok_or_else(|| fail("invalid audio resource value"))?;
            if !checked.insert(name.to_owned()) {
                continue;
            }
            if category == "movies" {
                let b = asset(&assets, name)?;
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
                continue;
            }
            if category == "streams" {
                check_wave(&assets, name)?;
                continue;
            }
            let document: Value = serde_json::from_slice(&asset(&assets, name)?)?;
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
                        &assets,
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
                    if song["division"].as_u64().is_none_or(|n| n == 0)
                        || !song["events"].is_array()
                    {
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
        }
    }
    fn audit(dir: &Path) -> CoreResult<()> {
        for entry in fs::read_dir(dir)? {
            let p = entry?.path();
            if p.is_dir() {
                audit(&p)?;
            } else {
                let name = p
                    .file_name()
                    .unwrap()
                    .to_string_lossy()
                    .to_ascii_lowercase();
                if ["snd", "hd", "bd", "sq", "mid", "wav", "pss"]
                    .contains(&name.rsplit('.').next().unwrap_or(""))
                    || name == "sound.dat"
                    || name == "sound.hd3"
                    || (p.parent().is_some_and(|parent| parent.ends_with("snd2"))
                        && matches!(name.as_str(), "rev.bin" | "map_index.txt"))
                {
                    return Err(fail(format!("retail audio remains: {}", p.display())));
                }
            }
        }
        Ok(())
    }
    if root.join("data").is_dir() {
        audit(&root.join("data"))
    } else {
        Ok(())
    }
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
        let mut files = BTreeMap::new();
        collect(&raw, &raw, &mut files).unwrap();
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
            serde_json::from_slice(&fs::read(output.join("port_data/snd2/rev.json")).unwrap())
                .unwrap();
        let routes: Value = serde_json::from_slice(
            &fs::read(output.join("port_data/snd2/map_index.json")).unwrap(),
        )
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
        fs::write(output.join("port_data/snd2/rev.json"), b"invalid JSON").unwrap();
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
        let d: Value = serde_json::from_slice(
            &fs::read(stage.join("port_data/test.sound/bank.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(d["programs"][0]["splits"][0]["bend_high"], 384);
        assert_eq!(d["samples"][0]["adsr1"], 0x80ff);
        assert_eq!(d["vags"][0]["loop_end"], 28);
        assert_eq!(d["vags"][0]["loops"], true);
        fs::write(stage.join("port_data/test.sound/samples/0.wav"), b"bad").unwrap();
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
            serde_json::from_slice(&fs::read(stage.join("port_data/test.sound.json")).unwrap())
                .unwrap();
        assert_eq!(
            bundle["effect_sequences"],
            json!(["test.sound/effects/z.json", "test.sound/effects/a.json"])
        );
        let config: Value = serde_json::from_slice(
            &fs::read(stage.join("port_data/test.sound/config.json")).unwrap(),
        )
        .unwrap();
        assert_eq!(
            config["entries"][0]["filename"],
            "test.sound/effects/z.json"
        );
        assert!(!stage.join("data/test.snd").exists());
        assert!(!stage.join("port_data/audio_catalog.json").exists());
        fs::remove_file(stage.join("port_data/test.sound/effects/z.json")).unwrap();
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
