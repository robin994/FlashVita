use std::alloc::{GlobalAlloc, Layout};
use std::ffi::c_void;

unsafe extern "C" {
    fn flashvita_vita_rust_alloc(bytes: usize, alignment: usize) -> *mut c_void;
    fn flashvita_vita_rust_alloc_zeroed(bytes: usize, alignment: usize) -> *mut c_void;
    fn flashvita_vita_rust_realloc(
        ptr: *mut c_void,
        old_size: usize,
        alignment: usize,
        new_size: usize,
    ) -> *mut c_void;
    fn flashvita_vita_rust_dealloc(ptr: *mut c_void);
}

pub struct VitaGlobalAllocator;

unsafe impl GlobalAlloc for VitaGlobalAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        flashvita_vita_rust_alloc(layout.size(), layout.align()).cast()
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        flashvita_vita_rust_alloc_zeroed(layout.size(), layout.align()).cast()
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        flashvita_vita_rust_dealloc(ptr.cast());
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        flashvita_vita_rust_realloc(
            ptr.cast(),
            layout.size(),
            layout.align(),
            new_size,
        )
        .cast()
    }
}
