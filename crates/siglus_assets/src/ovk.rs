//! OVK (Ogg/Vorbis pack) and OWP (XORed Ogg) helpers.
//!
//! Helpers for OVK (Ogg/Vorbis pack) and OWP (XORed Ogg) audio formats.

use crate::ogg_xor::{BoundedFile, validate_subrange};
use crate::vorbis;
use anyhow::{Result, bail};
use std::collections::HashMap;
use std::fs::File;
use std::io::{Cursor, Read};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Copy)]
pub struct OvkEntry {
    pub size: u32,
    pub offset: u32,
    pub no: u32,
    pub sample_count: u32,
}

#[derive(Debug, Clone)]
pub struct OvkPack {
    path: PathBuf,
    entries: Arc<Vec<OvkEntry>>,
    file_len: u64,
}

type OvkHeaderCache = HashMap<PathBuf, (Arc<Vec<OvkEntry>>, u64)>;

static OVK_HEADER_CACHE: Mutex<Option<OvkHeaderCache>> = Mutex::new(None);

impl OvkPack {
    pub fn open<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        if let Ok(mut guard) = OVK_HEADER_CACHE.lock() {
            let cache = guard.get_or_insert_with(HashMap::new);
            if let Some((entries, file_len)) = cache.get(&path) {
                return Ok(Self {
                    path,
                    entries: Arc::clone(entries),
                    file_len: *file_len,
                });
            }
        }

        let mut f = File::open(&path)?;
        let file_len = f.metadata()?.len();

        let mut head = [0u8; 4];
        f.read_exact(&mut head)?;
        let count = u32::from_le_bytes(head) as usize;
        if count == 0 {
            bail!("OVK: zero entries");
        }
        let mut table_bytes = vec![0u8; count * 16];
        f.read_exact(&mut table_bytes)?;
        let mut entries = Vec::with_capacity(count);
        for buf in table_bytes.chunks_exact(16) {
            let size = u32::from_le_bytes(buf[0..4].try_into().unwrap());
            let offset = u32::from_le_bytes(buf[4..8].try_into().unwrap());
            let no = u32::from_le_bytes(buf[8..12].try_into().unwrap());
            let smp = u32::from_le_bytes(buf[12..16].try_into().unwrap());
            entries.push(OvkEntry {
                size,
                offset,
                no,
                sample_count: smp,
            });
        }
        // Basic bounds checks (size==0 is allowed but pointless; treat as empty).
        for (i, e) in entries.iter().enumerate() {
            if e.size == 0 {
                continue;
            }
            validate_subrange(file_len, e.offset as u64, e.size as u64)
                .map_err(|err| anyhow::anyhow!("OVK entry[{i}] out of range: {err}"))?;
        }

        let entries = Arc::new(entries);
        if let Ok(mut guard) = OVK_HEADER_CACHE.lock() {
            let cache = guard.get_or_insert_with(HashMap::new);
            cache.insert(path.clone(), (Arc::clone(&entries), file_len));
        }

        Ok(Self {
            path,
            entries,
            file_len,
        })
    }

    pub fn entries(&self) -> &[OvkEntry] {
        &self.entries
    }

    pub fn get(&self, idx: usize) -> Option<OvkEntry> {
        self.entries.get(idx).copied()
    }

    /// Create a bounded reader for an entry.
    pub fn open_entry_stream(&self, idx: usize) -> Result<BoundedFile> {
        let e = self
            .entries
            .get(idx)
            .copied()
            .ok_or_else(|| anyhow::anyhow!("OVK: entry index out of range: {idx}"))?;
        if e.size == 0 {
            bail!("OVK: entry[{idx}] has zero size");
        }
        BoundedFile::open(&self.path, e.offset as u64, e.size as u64, None)
    }

    /// Extract an entry into memory.
    pub fn extract_entry(&self, idx: usize) -> Result<Vec<u8>> {
        self.open_entry_stream(idx)?.read_all()
    }

    /// Decode an entry (expected to be Ogg/Vorbis) into interleaved PCM16.
    pub fn decode_entry_vorbis_pcm16(&self, idx: usize) -> Result<vorbis::Pcm16> {
        let bytes = self.extract_entry(idx)?;
        vorbis::decode_ogg_vorbis_reader(Cursor::new(bytes))
    }

    /// Decode an entry (expected to be Ogg/Vorbis) and return a WAV (PCM16) buffer.
    pub fn decode_entry_vorbis_wav(&self, idx: usize) -> Result<Vec<u8>> {
        let bytes = self.extract_entry(idx)?;
        vorbis::decode_ogg_vorbis_reader_to_wav(Cursor::new(bytes))
    }
}

/// OWP: XOR-obfuscated Ogg file. The original engine uses key 0x39.
#[derive(Debug, Clone)]
pub struct OwpFile {
    path: PathBuf,
    file_len: u64,
    pub xor_key: u8,
}

impl OwpFile {
    pub const DEFAULT_XOR_KEY: u8 = 0x39;

    pub fn open<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let file_len = File::open(&path)?.metadata()?.len();
        Ok(Self {
            path,
            file_len,
            xor_key: Self::DEFAULT_XOR_KEY,
        })
    }

    pub fn open_stream(&self) -> Result<BoundedFile> {
        BoundedFile::open(&self.path, 0, self.file_len, Some(self.xor_key))
    }

    pub fn decrypt_to_vec(&self) -> Result<Vec<u8>> {
        self.open_stream()?.read_all()
    }

    /// Decode the XORed Ogg/Vorbis file into interleaved PCM16.
    pub fn decode_vorbis_pcm16(&self) -> Result<vorbis::Pcm16> {
        let bytes = self.decrypt_to_vec()?;
        vorbis::decode_ogg_vorbis_reader(Cursor::new(bytes))
    }

    /// Decode the XORed Ogg/Vorbis file and return a WAV (PCM16) buffer.
    pub fn decode_vorbis_wav(&self) -> Result<Vec<u8>> {
        let bytes = self.decrypt_to_vec()?;
        vorbis::decode_ogg_vorbis_reader_to_wav(Cursor::new(bytes))
    }
}
