/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     google/protobuf/timestamp.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#ifndef GOOGLE_PROTOBUF_TIMESTAMP_PROTO_UPB_H_
#define GOOGLE_PROTOBUF_TIMESTAMP_PROTO_UPB_H_

#include "upb/msg_internal.h"
#include "upb/decode.h"
#include "upb/decode_fast.h"
#include "upb/encode.h"

#include "upb/port_def.inc"

#ifdef __cplusplus
extern "C" {
#endif

struct google_protobuf_Timestamp;
typedef struct google_protobuf_Timestamp google_protobuf_Timestamp;
extern const upb_MiniTable google_protobuf_Timestamp_msginit;



/* google.protobuf.Timestamp */

UPB_INLINE google_protobuf_Timestamp* google_protobuf_Timestamp_new(upb_Arena* arena) {
  return (google_protobuf_Timestamp*)_upb_Message_New(&google_protobuf_Timestamp_msginit, arena);
}
UPB_INLINE google_protobuf_Timestamp* google_protobuf_Timestamp_parse(const char* buf, size_t size, upb_Arena* arena) {
  google_protobuf_Timestamp* ret = google_protobuf_Timestamp_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &google_protobuf_Timestamp_msginit, NULL, 0, arena) != kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE google_protobuf_Timestamp* google_protobuf_Timestamp_parse_ex(const char* buf, size_t size,
                           const upb_ExtensionRegistry* extreg,
                           int options, upb_Arena* arena) {
  google_protobuf_Timestamp* ret = google_protobuf_Timestamp_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &google_protobuf_Timestamp_msginit, extreg, options, arena) !=
      kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE char* google_protobuf_Timestamp_serialize(const google_protobuf_Timestamp* msg, upb_Arena* arena, size_t* len) {
  return upb_Encode(msg, &google_protobuf_Timestamp_msginit, 0, arena, len);
}
UPB_INLINE char* google_protobuf_Timestamp_serialize_ex(const google_protobuf_Timestamp* msg, int options,
                                 upb_Arena* arena, size_t* len) {
  return upb_Encode(msg, &google_protobuf_Timestamp_msginit, options, arena, len);
}
UPB_INLINE int64_t google_protobuf_Timestamp_seconds(const google_protobuf_Timestamp* msg) {
  return *UPB_PTR_AT(msg, UPB_SIZE(0, 0), int64_t);
}
UPB_INLINE int32_t google_protobuf_Timestamp_nanos(const google_protobuf_Timestamp* msg) {
  return *UPB_PTR_AT(msg, UPB_SIZE(8, 8), int32_t);
}

UPB_INLINE void google_protobuf_Timestamp_set_seconds(google_protobuf_Timestamp *msg, int64_t value) {
  *UPB_PTR_AT(msg, UPB_SIZE(0, 0), int64_t) = value;
}
UPB_INLINE void google_protobuf_Timestamp_set_nanos(google_protobuf_Timestamp *msg, int32_t value) {
  *UPB_PTR_AT(msg, UPB_SIZE(8, 8), int32_t) = value;
}

extern const upb_MiniTable_File google_protobuf_timestamp_proto_upb_file_layout;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#include "upb/port_undef.inc"

#endif  /* GOOGLE_PROTOBUF_TIMESTAMP_PROTO_UPB_H_ */
