//! `youyou play`: Annex-B clip to access units, for the offline decode/display self-test.

/// Splits an Annex-B stream into access units: the stateful V4L2 decoder takes
/// one picture per buffer, as the network path delivers them.
pub fn access_units(data: &[u8], hevc: bool) -> Vec<&[u8]> {
    let mut units = Vec::new();
    let (mut start, mut has_picture) = (0, false);
    let mut i = 0;
    while i + 3 < data.len() {
        if data[i..i + 3] != [0, 0, 1] {
            i += 1;
            continue;
        }
        let begin = if i > 0 && data[i - 1] == 0 { i - 1 } else { i };
        let nal = &data[i + 3..];
        i += 3;
        // A unit starts at parameter sets, AUD or prefix SEI, or at the first
        // slice of a picture (first_mb_in_slice == 0 / first_slice_segment_in_pic_flag).
        let (slice, opens) = if hevc {
            let kind = nal[0] >> 1 & 0x3f;
            (kind < 32, matches!(kind, 32..=35 | 39 | 41..=44 | 48..=55) || kind < 32 && nal.get(2).is_some_and(|b| b & 0x80 != 0))
        } else {
            let kind = nal[0] & 0x1f;
            (matches!(kind, 1 | 5), matches!(kind, 6..=9 | 14..=18) || matches!(kind, 1 | 5) && nal.get(1).is_some_and(|b| b & 0x80 != 0))
        };
        if has_picture && opens {
            units.push(&data[start..begin]);
            (start, has_picture) = (begin, false);
        }
        has_picture |= slice;
    }
    if has_picture {
        units.push(&data[start..]);
    }
    units
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn splits_h264_and_hevc_pictures() {
        // SPS PPS IDR(first slice) slice(second slice) | P(first slice) | AUD P(first slice)
        let h264 = [
            &[0, 0, 0, 1, 0x67, 1][..], &[0, 0, 1, 0x68, 1], &[0, 0, 1, 0x65, 0x88], &[0, 0, 1, 0x65, 0x40],
            &[0, 0, 1, 0x41, 0x9a], &[0, 0, 0, 1, 0x09, 0x10], &[0, 0, 1, 0x41, 0x9b],
        ]
        .concat();
        let units = access_units(&h264, false);
        assert_eq!(units.len(), 3);
        assert_eq!(units[0].len(), 6 + 5 + 5 + 5);
        assert_eq!(units[2], &[0, 0, 0, 1, 0x09, 0x10, 0, 0, 1, 0x41, 0x9b]);
        // VPS SPS PPS CRA(first) | TRAIL(first) TRAIL(second)
        let hevc = [
            &[0, 0, 0, 1, 0x40, 1, 0][..], &[0, 0, 1, 0x42, 1, 0], &[0, 0, 1, 0x44, 1, 0], &[0, 0, 1, 0x2a, 1, 0x80],
            &[0, 0, 1, 0x02, 1, 0x80], &[0, 0, 1, 0x02, 1, 0x40],
        ]
        .concat();
        let units = access_units(&hevc, true);
        assert_eq!(units.len(), 2);
        assert_eq!(units[1], &[0, 0, 1, 0x02, 1, 0x80, 0, 0, 1, 0x02, 1, 0x40]);
    }
}
