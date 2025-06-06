use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::core::gl::*;
use crate::core::platform::*;

use mesa_rust::pipe::screen::UUID_SIZE;
use mesa_rust_util::properties::Properties;
use rusticl_opencl_gen::*;
use rusticl_proc_macros::cl_entrypoint;
use rusticl_proc_macros::cl_info_entrypoint;

use std::ffi::c_char;
use std::ffi::c_void;
use std::mem::transmute;
use std::ptr;
use std::slice;

#[cl_info_entrypoint(clGetContextInfo)]
unsafe impl CLInfo<cl_context_info> for cl_context {
    fn query(&self, q: cl_context_info, v: CLInfoValue) -> CLResult<CLInfoRes> {
        let ctx = Context::ref_from_raw(*self)?;
        match q {
            CL_CONTEXT_DEVICES => v.write::<Vec<cl_device_id>>(
                ctx.devs
                    .iter()
                    .map(|&d| cl_device_id::from_ptr(d))
                    .collect(),
            ),
            CL_CONTEXT_NUM_DEVICES => v.write::<cl_uint>(ctx.devs.len() as u32),
            // need to return None if no properties exist
            CL_CONTEXT_PROPERTIES => v.write::<&Properties<cl_context_properties>>(&ctx.properties),
            CL_CONTEXT_REFERENCE_COUNT => v.write::<cl_uint>(Context::refcnt(*self)?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => Err(CL_INVALID_VALUE),
        }
    }
}

unsafe impl CLInfo<cl_gl_context_info> for GLCtxManager {
    fn query(&self, q: cl_gl_context_info, v: CLInfoValue) -> CLResult<CLInfoRes> {
        let info = self.interop_dev_info;

        match q {
            CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR => {
                let ptr = match get_dev_for_uuid(info.device_uuid) {
                    Some(dev) => dev,
                    None => ptr::null(),
                };
                v.write::<cl_device_id>(cl_device_id::from_ptr(ptr))
            }
            CL_DEVICES_FOR_GL_CONTEXT_KHR => {
                // TODO: support multiple devices
                v.write_iter::<cl_device_id>(
                    get_dev_for_uuid(info.device_uuid)
                        .iter()
                        .map(|&d| cl_device_id::from_ptr(d)),
                )
            }
            _ => Err(CL_INVALID_VALUE),
        }
    }
}

#[cl_entrypoint(clGetGLContextInfoKHR)]
pub fn get_gl_context_info_khr(
    properties: *const cl_context_properties,
    param_name: cl_gl_context_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> CLResult<()> {
    let mut egl_display: EGLDisplay = ptr::null_mut();
    let mut glx_display: *mut _XDisplay = ptr::null_mut();
    let mut gl_context: *mut c_void = ptr::null_mut();

    // CL_INVALID_PROPERTY [...] if the same property name is specified more than once.
    // SAFETY: properties is a 0 terminated array by spec.
    let props = unsafe { Properties::new(properties) }.ok_or(CL_INVALID_PROPERTY)?;
    for (&key, &val) in props.iter() {
        let key = u32::try_from(key).or(Err(CL_INVALID_PROPERTY))?;
        match key {
            // CL_INVALID_PLATFORM [...] if platform value specified in properties is not a valid platform.
            CL_CONTEXT_PLATFORM => {
                (val as cl_platform_id).get_ref()?;
            }
            CL_EGL_DISPLAY_KHR => {
                egl_display = val as *mut _;
            }
            CL_GL_CONTEXT_KHR => {
                gl_context = val as *mut _;
            }
            CL_GLX_DISPLAY_KHR => {
                glx_display = val as *mut _;
            }
            // CL_INVALID_PROPERTY if context property name in properties is not a supported property name
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    let gl_ctx_manager = GLCtxManager::new(gl_context, glx_display, egl_display)?;
    unsafe {
        gl_ctx_manager
            .ok_or(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)?
            .get_info(
                param_name,
                param_value_size,
                param_value,
                param_value_size_ret,
            )
    }
}

#[cl_entrypoint(clCreateContext)]
fn create_context(
    properties: *const cl_context_properties,
    num_devices: cl_uint,
    devices: *const cl_device_id,
    pfn_notify: Option<FuncCreateContextCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<cl_context> {
    // TODO: Actually hook this callback up so it gets called when appropriate.
    // SAFETY: The requirements on `CreateContextCB::try_new` match the requirements
    // imposed by the OpenCL specification. It is the caller's duty to uphold them.
    let _cb_opt = unsafe { CreateContextCB::try_new(pfn_notify, user_data)? };

    // CL_INVALID_VALUE if devices is NULL.
    if devices.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE if num_devices is equal to zero.
    if num_devices == 0 {
        return Err(CL_INVALID_VALUE);
    }

    let mut egl_display: EGLDisplay = ptr::null_mut();
    let mut glx_display: *mut _XDisplay = ptr::null_mut();
    let mut gl_context: *mut c_void = ptr::null_mut();

    // CL_INVALID_PROPERTY [...] if the same property name is specified more than once.
    // SAFETY: properties is a 0 terminated array by spec.
    let props = unsafe { Properties::new(properties) }.ok_or(CL_INVALID_PROPERTY)?;
    for (&key, &val) in props.iter() {
        let key = u32::try_from(key).or(Err(CL_INVALID_PROPERTY))?;
        match key {
            // CL_INVALID_PLATFORM [...] if platform value specified in properties is not a valid platform.
            CL_CONTEXT_PLATFORM => {
                (val as cl_platform_id).get_ref()?;
            }
            CL_CONTEXT_INTEROP_USER_SYNC => {
                check_cl_bool(val).ok_or(CL_INVALID_PROPERTY)?;
            }
            CL_EGL_DISPLAY_KHR => {
                egl_display = val as *mut _;
            }
            CL_GL_CONTEXT_KHR => {
                gl_context = val as *mut _;
            }
            CL_GLX_DISPLAY_KHR => {
                glx_display = val as *mut _;
            }
            // CL_INVALID_PROPERTY if context property name in properties is not a supported property name
            _ => return Err(CL_INVALID_PROPERTY),
        }
    }

    // Duplicate devices specified in devices are ignored.
    let mut devs = unsafe { slice::from_raw_parts(devices, num_devices as usize) }.to_owned();
    devs.sort();
    devs.dedup();
    let devs: Result<_, _> = devs.into_iter().map(Device::ref_from_raw).collect();
    let devs: Vec<&Device> = devs?;

    let gl_ctx_manager = GLCtxManager::new(gl_context, glx_display, egl_display)?;
    if let Some(gl_ctx_manager) = &gl_ctx_manager {
        // errcode_ret returns CL_INVALID_OPERATION if a context was specified as described above
        // and any of the following conditions hold:
        // ...
        // Any of the devices specified in the devices argument cannot support OpenCL objects which
        // share the data store of an OpenGL object.

        let [dev] = devs.as_slice() else {
            return Err(CL_INVALID_OPERATION);
        };

        if !dev.is_gl_sharing_supported() {
            return Err(CL_INVALID_OPERATION);
        }

        // gl sharing is only supported on devices with an UUID, so we can simply unwrap it
        let dev_uuid: [c_char; UUID_SIZE] =
            unsafe { transmute(dev.screen().device_uuid().unwrap_or_default()) };
        if gl_ctx_manager.interop_dev_info.device_uuid != dev_uuid {
            // we only support gl_sharing on the same device
            return Err(CL_INVALID_OPERATION);
        }
    }

    Ok(Context::new(devs, props, gl_ctx_manager).into_cl())
}

#[cl_entrypoint(clCreateContextFromType)]
fn create_context_from_type(
    properties: *const cl_context_properties,
    device_type: cl_device_type,
    pfn_notify: Option<FuncCreateContextCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<cl_context> {
    // CL_INVALID_DEVICE_TYPE if device_type is not a valid value.
    check_cl_device_type(device_type)?;

    let devs: Vec<_> = get_devs_for_type(device_type)
        .into_iter()
        .map(|d| cl_device_id::from_ptr(d))
        .collect();

    // CL_DEVICE_NOT_FOUND if no devices that match device_type and property values specified in properties were found.
    if devs.is_empty() {
        return Err(CL_DEVICE_NOT_FOUND);
    }

    // errors are essentially the same and we will always pass in a valid
    // device list, so that's fine as well.
    create_context(
        properties,
        devs.len() as u32,
        devs.as_ptr(),
        pfn_notify,
        user_data,
    )
}

#[cl_entrypoint(clRetainContext)]
fn retain_context(context: cl_context) -> CLResult<()> {
    Context::retain(context)
}

#[cl_entrypoint(clReleaseContext)]
fn release_context(context: cl_context) -> CLResult<()> {
    Context::release(context)
}

#[cl_entrypoint(clSetContextDestructorCallback)]
fn set_context_destructor_callback(
    context: cl_context,
    pfn_notify: ::std::option::Option<FuncDeleteContextCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> CLResult<()> {
    let c = Context::ref_from_raw(context)?;

    // SAFETY: The requirements on `DeleteContextCB::new` match the requirements
    // imposed by the OpenCL specification. It is the caller's duty to uphold them.
    let cb = unsafe { DeleteContextCB::new(pfn_notify, user_data)? };

    c.dtors.lock().unwrap().push(cb);
    Ok(())
}
