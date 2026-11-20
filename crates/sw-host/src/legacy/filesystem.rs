//! Read-only access to both LittleFS 2.0 (public firmware) and 2.1 (factory images).
use anyhow::{Result, ensure};
use littlefs2_sys as lfs;
use std::ffi::{CStr, c_void};

const BLOCK: usize = 4096;
unsafe extern "C" fn read_block(
    config: *const lfs::lfs_config,
    block: u32,
    offset: u32,
    out: *mut c_void,
    size: u32,
) -> i32 {
    // mounted keeps the slice and config alive for the entire synchronous call.
    let bytes = unsafe { &*((*config).context.cast::<&[u8]>()) };
    let start = u64::from(block) * BLOCK as u64 + u64::from(offset);
    let end = start + u64::from(size);
    if end > bytes.len() as u64 {
        return lfs::lfs_error_LFS_ERR_IO;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(
            bytes.as_ptr().add(start as usize),
            out.cast(),
            size as usize,
        )
    };
    0
}

pub(super) struct Volume<'a>(&'a mut lfs::lfs_t);
impl Drop for Volume<'_> {
    fn drop(&mut self) {
        unsafe { lfs::lfs_unmount(self.0) };
    }
}

pub(super) fn mounted<T>(bytes: &[u8], inspect: impl FnOnce(&mut Volume<'_>) -> T) -> Option<T> {
    // The C API takes zero-initialized structs. Its default disk version accepts
    // both 2.x formats; the higher-level Rust wrapper pins mounting to 2.0.
    let mut state = unsafe { std::mem::zeroed() };
    let config = lfs::lfs_config {
        context: std::ptr::from_ref(&bytes).cast_mut().cast(),
        read: Some(read_block),
        read_size: 16,
        prog_size: 256,
        block_size: BLOCK as u32,
        block_count: (bytes.len() / BLOCK) as u32,
        block_cycles: -1,
        cache_size: 512,
        lookahead_size: 16,
        ..unsafe { std::mem::zeroed() }
    };
    if unsafe { lfs::lfs_mount(&mut state, &config) } != 0 {
        return None;
    }
    Some(inspect(&mut Volume(&mut state)))
}

impl Volume<'_> {
    pub fn read(&mut self, name: &CStr, limit: usize) -> Result<Option<Vec<u8>>> {
        let mut file = unsafe { std::mem::zeroed() };
        let status = unsafe {
            lfs::lfs_file_open(
                self.0,
                &mut file,
                name.as_ptr(),
                lfs::lfs_open_flags_LFS_O_RDONLY as _,
            )
        };
        if status == lfs::lfs_error_LFS_ERR_NOENT {
            return Ok(None);
        }
        ensure!(status == 0, "Cannot open the older save file");
        let result = (|| {
            let size = unsafe { lfs::lfs_file_size(self.0, &mut file) };
            ensure!(
                size >= 0 && size as usize <= limit,
                "Invalid legacy save size"
            );
            let mut bytes = vec![0; size as usize];
            let count = unsafe {
                lfs::lfs_file_read(self.0, &mut file, bytes.as_mut_ptr().cast(), size as u32)
            };
            ensure!(count == size, "Cannot read the older save file");
            Ok(Some(bytes))
        })();
        unsafe { lfs::lfs_file_close(self.0, &mut file) };
        result
    }
}
