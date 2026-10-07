use crate::events::{Ev, Reporter};
use crate::util::{crc32, err_text, exe_path, format_bytes, show};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

const TRAILER_MAGIC: u32 = 0x5359_5054;
const ARCHIVE_MAGIC: u32 = 0x5359_4152;
const FORMAT_VERSION: u32 = 1;
const TRAILER_SIZE: u64 = 24;

struct Trailer {
    archive_offset: u64,
    archive_size: u64,
    magic: u32,
    version: u32,
}

struct Header {
    file_count: u32,
    total_uncompressed: u64,
}

struct Block {
    compressed: u32,
    raw: u32,
    stored: bool,
}

struct Entry {
    name: String,
    size: u64,
    crc: u32,
    blocks: Vec<Block>,
}

impl Entry {
    fn compressed_bytes(&self) -> u64 {
        self.blocks.iter().map(|b| b.compressed as u64).sum()
    }
}

fn u32le(f: &mut File) -> std::io::Result<u32> {
    let mut b = [0u8; 4];
    f.read_exact(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

fn u64le(f: &mut File) -> std::io::Result<u64> {
    let mut b = [0u8; 8];
    f.read_exact(&mut b)?;
    Ok(u64::from_le_bytes(b))
}

fn read_trailer(f: &mut File, size: u64) -> Option<Trailer> {
    if size < TRAILER_SIZE {
        return None;
    }
    f.seek(SeekFrom::Start(size - TRAILER_SIZE)).ok()?;
    Some(Trailer {
        archive_offset: u64le(f).ok()?,
        archive_size: u64le(f).ok()?,
        magic: u32le(f).ok()?,
        version: u32le(f).ok()?,
    })
}

fn payload_source() -> std::path::PathBuf {
    #[cfg(debug_assertions)]
    if let Some(p) = std::env::var_os("SYMPATHY_PAYLOAD_EXE") {
        return p.into();
    }
    exe_path()
}

fn open_self() -> Result<(File, Header), String> {
    let me = payload_source();
    let mut f = File::open(&me).map_err(|e| format!("cannot open {}: {}", show(&me), err_text(&e)))?;
    let size = f.metadata().map(|m| m.len()).unwrap_or(0);
    let tr = read_trailer(&mut f, size).ok_or("setup.exe is too small to carry a payload")?;
    if tr.magic != TRAILER_MAGIC {
        return Err("this setup.exe carries no payload -- it was not packed".into());
    }
    if tr.version != FORMAT_VERSION {
        return Err("payload format version is not the one this build understands".into());
    }
    if tr.archive_offset + tr.archive_size + TRAILER_SIZE != size {
        return Err("setup.exe is truncated or was modified after packing".into());
    }
    f.seek(SeekFrom::Start(tr.archive_offset)).map_err(|_| "cannot read the archive header")?;
    let magic = u32le(&mut f).map_err(|_| "cannot read the archive header")?;
    let version = u32le(&mut f).map_err(|_| "cannot read the archive header")?;
    let file_count = u32le(&mut f).map_err(|_| "cannot read the archive header")?;
    let _block_size = u32le(&mut f).map_err(|_| "cannot read the archive header")?;
    let total_uncompressed = u64le(&mut f).map_err(|_| "cannot read the archive header")?;
    if magic != ARCHIVE_MAGIC || version != FORMAT_VERSION {
        return Err("archive header is not valid".into());
    }
    Ok((f, Header { file_count, total_uncompressed }))
}

fn read_entry(f: &mut File) -> Result<Entry, String> {
    let name_bytes = u32le(f).map_err(|_| "archive entry has a bad name length")?;
    if name_bytes == 0 || name_bytes > 1024 {
        return Err("archive entry has a bad name length".into());
    }
    let mut name = vec![0u8; name_bytes as usize];
    f.read_exact(&mut name).map_err(|_| "cannot read an archive entry name")?;
    let size = u64le(f).map_err(|_| "cannot read an archive entry header")?;
    let crc = u32le(f).map_err(|_| "cannot read an archive entry header")?;
    let count = u32le(f).map_err(|_| "cannot read an archive entry header")?;
    let mut blocks = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let compressed = u32le(f).map_err(|_| "cannot read the block table")?;
        let raw = u32le(f).map_err(|_| "cannot read the block table")?;
        let stored = u32le(f).map_err(|_| "cannot read the block table")? != 0;
        blocks.push(Block { compressed, raw, stored });
    }
    let name = String::from_utf8_lossy(&name).to_string();
    if name.contains('\\') || name.contains('/') || name.contains(':') || name == ".." {
        return Err(format!("archive entry name is not a plain file name: {}", name));
    }
    Ok(Entry { name, size, crc, blocks })
}

pub fn has_payload() -> Option<u64> {
    open_self().ok().map(|(_, h)| h.total_uncompressed)
}

pub fn list_payload() -> Option<Vec<String>> {
    let (mut f, h) = open_self().ok()?;
    let mut names = Vec::new();
    for _ in 0..h.file_count {
        let e = read_entry(&mut f).ok()?;
        f.seek(SeekFrom::Current(e.compressed_bytes() as i64)).ok()?;
        names.push(e.name);
    }
    Some(names)
}

pub fn write_uninstaller_copy(dest: &Path) -> Result<(), String> {
    let me = exe_path();
    let mut f = File::open(&me).map_err(|e| format!("cannot open {}: {}", show(&me), err_text(&e)))?;
    let size = f.metadata().map(|m| m.len()).map_err(|_| format!("cannot size {}", show(&me)))?;
    let code = match read_trailer(&mut f, size) {
        Some(t) if t.magic == TRAILER_MAGIC => t.archive_offset,
        _ => size,
    };
    f.seek(SeekFrom::Start(0)).map_err(|e| format!("copy failed: {}", err_text(&e)))?;
    let mut out = File::create(dest).map_err(|e| format!("cannot create {}: {}", show(dest), err_text(&e)))?;
    let r = std::io::copy(&mut (&mut f).take(code), &mut out);
    match r {
        Ok(n) if n == code => Ok(()),
        Ok(_) => {
            drop(out);
            let _ = std::fs::remove_file(dest);
            Err("copy failed: short read".into())
        }
        Err(e) => {
            drop(out);
            let _ = std::fs::remove_file(dest);
            Err(format!("copy failed: {}", err_text(&e)))
        }
    }
}

struct Decompressor(windows::Win32::Storage::Compression::DECOMPRESSOR_HANDLE);

impl Drop for Decompressor {
    fn drop(&mut self) {
        unsafe {
            let _ = windows::Win32::Storage::Compression::CloseDecompressor(self.0);
        }
    }
}

pub fn extract_payload(dest: &Path, rep: &dyn Reporter, skip: &str) -> bool {
    use windows::Win32::Storage::Compression::{CreateDecompressor, Decompress, COMPRESS_ALGORITHM_LZMS, DECOMPRESSOR_HANDLE};

    let (mut f, h) = match open_self() {
        Ok(x) => x,
        Err(e) => {
            rep.ev(Ev::Failed { detail: format!("{}", e) });
            return false;
        }
    };

    let mut handle = DECOMPRESSOR_HANDLE::default();
    if let Err(e) = unsafe { CreateDecompressor(COMPRESS_ALGORITHM_LZMS, None, &mut handle) } {
        rep.ev(Ev::Failed { detail: format!("cannot start the decompressor: {}", e.message()) });
        return false;
    }
    let dec = Decompressor(handle);

    let mut comp: Vec<u8> = Vec::new();
    let mut raw: Vec<u8> = Vec::new();

    for _ in 0..h.file_count {
        let e = match read_entry(&mut f) {
            Ok(e) => e,
            Err(err) => {
                rep.ev(Ev::Failed { detail: format!("{}", err) });
                return false;
            }
        };

        if !skip.is_empty() && e.name.eq_ignore_ascii_case(skip) {
            if f.seek(SeekFrom::Current(e.compressed_bytes() as i64)).is_err() {
                rep.ev(Ev::Failed { detail: format!("cannot skip past {}", e.name) });
                return false;
            }
            continue;
        }

        let out_path = dest.join(&e.name);
        let mut out = match File::create(&out_path) {
            Ok(o) => o,
            Err(err) => {
                rep.ev(Ev::Failed { detail: format!("cannot create {}: {}", show(&out_path), err_text(&err)) });
                return false;
            }
        };

        let mut crc = 0u32;
        for b in &e.blocks {
            comp.resize(b.compressed as usize, 0);
            if f.read_exact(&mut comp).is_err() {
                rep.ev(Ev::Failed { detail: format!("cannot read compressed data for {}", e.name) });
                return false;
            }
            let data: &[u8] = if b.stored {
                &comp
            } else {
                raw.resize(b.raw as usize, 0);
                let mut produced: usize = 0;
                let r = unsafe {
                    Decompress(
                        dec.0,
                        Some(comp.as_ptr() as *const _),
                        comp.len(),
                        Some(raw.as_mut_ptr() as *mut _),
                        raw.len(),
                        Some(&mut produced),
                    )
                };
                if r.is_err() || produced != b.raw as usize {
                    let why = r.err().map(|x| x.message()).unwrap_or_else(|| "short output".into());
                    rep.ev(Ev::Failed { detail: format!("cannot decompress {}: {}", e.name, why) });
                    return false;
                }
                &raw
            };
            if let Err(err) = out.write_all(data) {
                rep.ev(Ev::Failed { detail: format!("cannot write {}: {}", show(&out_path), err_text(&err)) });
                return false;
            }
            crc = crc32(data, crc);
        }
        drop(out);

        if crc != e.crc {
            rep.ev(Ev::Failed {
                detail: format!(
                    "{} did not survive unpacking (checksum). the download is probably incomplete -- fetch setup.exe again.",
                    e.name
                ),
            });
            return false;
        }

        rep.ev(Ev::File { name: e.name.clone(), size: format_bytes(e.size) });
    }
    true
}
