// Small helpers every serving route shares: the steady clock the stats use, and
// the completion calls that honor XrdCl's native contract (a non-null, empty
// HostList on every synthesized completion — see complete() in UCacheFile.cc).
//
// Thread-safety: stateless.
#pragma once

#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>

namespace ucache {

uint64_t nowUs();
void complete(XrdCl::ResponseHandler* h, XrdCl::XRootDStatus* status, XrdCl::AnyObject* response);
XrdCl::XRootDStatus* okStatus();
XrdCl::AnyObject* chunkResponse(uint64_t off, uint32_t len, void* buf);
XrdCl::AnyObject* vreadResponse(const XrdCl::ChunkList& chunks);

} // namespace ucache
