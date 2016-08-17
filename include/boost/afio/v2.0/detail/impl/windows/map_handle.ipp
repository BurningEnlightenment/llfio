/* map_handle.hpp
A handle to a source of mapped memory
(C) 2016 Niall Douglas http://www.nedprod.com/
File Created: August 2016


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "../../../map_handle.hpp"
#include "../../../utils.hpp"
#include "import.hpp"

BOOST_AFIO_V2_NAMESPACE_BEGIN

result<section_handle> section_handle::section(file_handle &backing, extent_type maximum_size, mode _mode) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  if(!maximum_size && backing.is_valid())
  {
    BOOST_OUTCOME_FILTER_ERROR(length, backing.length());
    maximum_size = length;
  }
  maximum_size = utils::round_up_to_page_size(maximum_size);
  result<section_handle> ret(section_handle(native_handle_type(), backing.is_valid() ? &backing : nullptr, maximum_size, _mode));
  native_handle_type &nativeh = ret.get()._v;
  ACCESS_MASK access = SECTION_QUERY;
  ULONG prot = 0, attribs = 0;
  if(backing.is_valid())
    access |= SECTION_EXTEND_SIZE;
  if(_mode & mode::read)
  {
    access |= SECTION_MAP_READ;
    prot = PAGE_READONLY;
    nativeh.behaviour |= native_handle_type::disposition::readable;
  }
  if(_mode & mode::write)
  {
    access |= SECTION_MAP_WRITE;
    prot = PAGE_READWRITE;
    nativeh.behaviour |= native_handle_type::disposition::writable;
  }
  if(_mode & mode::execute)
  {
    access |= SECTION_MAP_EXECUTE;
    prot = PAGE_EXECUTE;
  }
  if(_mode & mode::cow)
    prot = PAGE_WRITECOPY;
  if(_mode & mode::nocommit)
    attribs = SEC_RESERVE;
  else
    attribs = SEC_COMMIT;
  if(_mode & mode::executable)
    attribs = SEC_IMAGE;
  if(_mode & mode::prefault)
  {
    // Handled during view mapping below
  }
  nativeh.behaviour |= native_handle_type::disposition::section;
  // OBJECT_ATTRIBUTES ObjectAttributes;
  // InitializeObjectAttributes(&ObjectAttributes, &NULL, 0, NULL, NULL);
  LARGE_INTEGER _maximum_size;
  _maximum_size.QuadPart = maximum_size;
  NTSTATUS ntstat = NtCreateSection(&nativeh.h, access, NULL, &_maximum_size, prot, attribs, backing.is_valid() ? backing.native_handle().h : NULL);
  if(STATUS_SUCCESS != ntstat)
    return make_errored_result_nt<section_handle>(ntstat);
  return ret;
}

result<section_handle::extent_type> section_handle::truncate(extent_type newsize) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  newsize = utils::round_up_to_page_size(newsize);
  LARGE_INTEGER _maximum_size;
  _maximum_size.QuadPart = newsize;
  NTSTATUS ntstat = NtExtendSection(_v.h, &_maximum_size);
  if(STATUS_SUCCESS != ntstat)
    return make_errored_result_nt<extent_type>(ntstat);
  _length = newsize;
  return make_result<extent_type>(newsize);
}


map_handle::~map_handle()
{
  if(_v)
  {
    // Unmap the view
    auto ret = map_handle::close();
    if(ret.has_error())
    {
      BOOST_AFIO_LOG_FATAL(_v.h, "map_handle::~map_handle() close failed");
    }
  }
}

result<void> map_handle::close() noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(_addr);
  if(_addr)
  {
    NTSTATUS ntstat = NtUnmapViewOfSection(GetCurrentProcess(), _addr);
    if(STATUS_SUCCESS != ntstat)
      return make_errored_result_nt<void>(ntstat);
  }
  // We don't want ~handle() to close our borrowed handle
  _v = native_handle_type();
  _addr = nullptr;
  _length = 0;
  return make_ready_result<void>();
}

native_handle_type map_handle::release() noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  // We don't want ~handle() to close our borrowed handle
  _v = native_handle_type();
  _addr = nullptr;
  _length = 0;
  return native_handle_type();
}


result<map_handle> map_handle::map(section_handle &section, size_type bytes, extent_type offset, section_handle::mode _mode) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(0);
  bytes = utils::round_up_to_page_size(bytes);
  result<map_handle> ret(map_handle(io_handle(), &section));
  native_handle_type &nativeh = ret.get()._v;
  ULONG allocation = (section.section_mode() & section_handle::mode::nocommit) ? MEM_RESERVE : 0, prot = 0;
  PVOID addr = 0;
  size_t commitsize = bytes;
  LARGE_INTEGER _offset;
  _offset.QuadPart = offset;
  SIZE_T _bytes = bytes;
  if(_mode == section_handle::mode::none)
  {
    allocation = MEM_RESERVE;
    commitsize = 0;
  }
  else if(_mode & section_handle::mode::cow)
  {
    prot = PAGE_WRITECOPY;
    nativeh.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable | native_handle_type::disposition::writable;
  }
  else if(_mode & section_handle::mode::write)
  {
    prot = PAGE_READWRITE;
    nativeh.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable | native_handle_type::disposition::writable;
  }
  else if(_mode & section_handle::mode::read)
  {
    prot = PAGE_READONLY;
    nativeh.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable;
  }
  if(_mode & section_handle::mode::execute)
    prot = PAGE_EXECUTE;
  NTSTATUS ntstat = NtMapViewOfSection(section.native_handle().h, GetCurrentProcess(), &addr, 0, commitsize, &_offset, &_bytes, ViewUnmap, allocation, prot);
  if(STATUS_SUCCESS != ntstat)
    return make_errored_result_nt<map_handle>(ntstat);
  ret.get()._addr = (char *) addr;
  ret.get()._length = _bytes;
  // Make my handle borrow the native handle of my backing storage
  ret.get()._v = section.backing_native_handle();

  // Windows has no way of getting the kernel to prefault maps on creation, so ...
  if(section.section_mode() & section_handle::mode::prefault)
  {
    // Start an asynchronous prefetch
    buffer_type b((char *) addr, _bytes);
    prefetch(span<buffer_type>(&b, 1));
    size_t pagesize = utils::page_size();
    // And follow it up with poking every page in the map
    volatile char *a = (volatile char *) addr;
    for(size_t n = 0; n < _bytes; n += pagesize)
      a[n];
  }
  return ret;
}

result<map_handle::buffer_type> map_handle::commit(buffer_type region, section_handle::mode _mode) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  DWORD prot = 0;
  if(_mode == section_handle::mode::none)
  {
    BOOST_OUTCOME_FILTER_ERROR(_region, dontneed(region));
    if(!VirtualProtect(_region.first, _region.second, PAGE_NOACCESS, NULL))
      return make_errored_result<buffer_type>(GetLastError());
    return _region;
  }
  if(_mode & section_handle::mode::cow)
  {
    prot = PAGE_WRITECOPY;
    _v.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable | native_handle_type::disposition::writable;
  }
  else if(_mode & section_handle::mode::write)
  {
    prot = PAGE_READWRITE;
    _v.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable | native_handle_type::disposition::writable;
  }
  else if(_mode & section_handle::mode::read)
  {
    prot = PAGE_READONLY;
    _v.behaviour |= native_handle_type::disposition::seekable | native_handle_type::disposition::readable;
  }
  if(_mode & section_handle::mode::execute)
    prot = PAGE_EXECUTE;
  region = utils::round_to_page_size(region);
  if(!VirtualAlloc(region.first, region.second, MEM_COMMIT, prot))
    return make_errored_result<buffer_type>(GetLastError());
  return region;
}

result<void> map_handle::zero(buffer_type region) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  //! \todo Once you implement file_handle::zero(), please implement map_handle::zero()
  // buffer_type page_region { (char *) utils::round_up_to_page_size((uintptr_t) region.first), utils::round_down_to_page_size(region.second); };
  memset(region.first, 0, region.second);
  return make_ready_result<void>();
}

result<span<map_handle::buffer_type>> map_handle::prefetch(span<buffer_type> regions) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(0);
  if(!PrefetchVirtualMemory_)
    return span<map_handle::buffer_type>();
  PWIN32_MEMORY_RANGE_ENTRY wmre = (PWIN32_MEMORY_RANGE_ENTRY) regions.data();
  if(!PrefetchVirtualMemory_(GetCurrentProcess(), regions.size(), wmre, 0))
    return make_errored_result<span<map_handle::buffer_type>>(GetLastError());
  return regions;
}

result<map_handle::buffer_type> map_handle::dontneed(buffer_type region) noexcept
{
  region = utils::round_to_page_size(region);
  if(!VirtualAlloc(region.first, region.second, MEM_RESET, 0))
    return make_errored_result<buffer_type>(GetLastError());
  return region;
}

map_handle::io_result<map_handle::buffers_type> map_handle::read(io_request<buffers_type> reqs, deadline) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  char *addr = _addr + reqs.offset;
  size_type togo = (size_type)(_length - reqs.offset);
  for(buffer_type &req : reqs.buffers)
  {
    if(togo)
    {
      req.first = addr;
      if(req.second < togo)
        req.second = togo;
      addr += req.second;
      togo -= req.second;
    }
    else
      req.second = 0;
  }
  return reqs.buffers;
}

map_handle::io_result<map_handle::const_buffers_type> map_handle::write(io_request<const_buffers_type> reqs, deadline) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  const char *addr = _addr + reqs.offset;
  size_type togo = (size_type)(_length - reqs.offset);
  for(const_buffer_type &req : reqs.buffers)
  {
    if(togo)
    {
      req.first = addr;
      if(req.second < togo)
        req.second = togo;
      addr += req.second;
      togo -= req.second;
    }
    else
      req.second = 0;
  }
  return reqs.buffers;
}

BOOST_AFIO_V2_NAMESPACE_END
