/* h264_sps — minimal H.264 SPS parser: extracts the actual coded picture
 * width/height (post-cropping) so ONVIF can advertise the real stream
 * resolution instead of a configured guess (substreams/sensors differ).
 */
#ifndef OKAM_H264_SPS_H
#define OKAM_H264_SPS_H

#include <stdint.h>
#include <stddef.h>

/* nal must be a full H.264 NAL unit (nal[0] is the NAL header byte, i.e.
 * pass exactly what vsc_iter_annexb_nals()/cam_source hand you -- no start
 * code, no RBSP-unescaping done by the caller). Returns 1 and fills
 * out_width and out_height on success, 0 if `nal` isn't an SPS (type 7) or
 * parsing failed/looked implausible (caller should keep its configured
 * default in that case -- this is best-effort metadata, not required for
 * the actual RTSP stream to work). */
int h264_sps_get_resolution(const uint8_t *nal, size_t len, int *out_width, int *out_height);

#endif /* OKAM_H264_SPS_H */
