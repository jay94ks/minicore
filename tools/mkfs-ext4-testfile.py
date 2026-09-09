#!/usr/bin/env python3
"""tools/mkfs-ext4-testfile.py — docs/plan/system-servers-bringup.md §M16
검증용 ext4 이미지에 파일 하나를 루트 디렉터리에 직접 써 넣는다.

이 프로젝트가 개발하는 호스트(Windows)에는 ext4를 실제로 마운트할
방법이 없다(WSL/관리자 권한 필요, 이 스크립트의 전제 밖) — 그리고
`mke2fs -d <디렉터리>`(디렉터리 내용으로 즉시 채우는 옵션)는 이
환경의 mke2fs 빌드에서 깨져 있다(경로 인코딩 버그로 추정, 직접
확인함). 그래서 `mke2fs`로 **빈** 클린 ext4 이미지를 만든 뒤, 이
스크립트가 그 온디스크 구조(슈퍼블록/그룹 디스크립터/비트맵/아이노드
테이블/extent 트리/디렉터리 엔트리)를 직접 읽고 고쳐 파일 하나를
추가한다 — servers/fs/ext4가 실제로 마운트해서 읽어낼 대상.

**전제(이 스크립트가 검증하지 않고 그냥 요구하는 것)**: 이미지가
`mke2fs -O ^has_journal,^64bit,^metadata_csum,^uninit_bg`로 만들어진,
단일 블록 그룹(4~8MiB 안팍의 작은 이미지)의 클린 상태 — ADR-129 v1
범위와 정확히 일치한다. 여러 그룹, flex_bg 메타데이터 재배치, 저널
있는 이미지는 다루지 않는다(이 스크립트의 목적은 "테스트 픽스처
하나 만들기"뿐, 범용 ext4 쓰기 도구가 아니다).

사용법:
    mkfs-ext4-testfile.py <이미지경로> <파일이름> <내용파일경로>
"""
import struct
import sys


def u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def set_u16(buf, off, val):
    struct.pack_into("<H", buf, off, val)


def set_u32(buf, off, val):
    struct.pack_into("<I", buf, off, val)


def main(argv):
    if len(argv) != 4:
        print(f"사용법: {argv[0]} <이미지경로> <파일이름> <내용파일경로>", file=sys.stderr)
        return 1
    image_path, file_name, content_path = argv[1], argv[2], argv[3]

    with open(content_path, "rb") as f:
        content = f.read()

    with open(image_path, "r+b") as f:
        data = bytearray(f.read())

        sb_off = 1024
        first_data_block = u32(data, sb_off + 20)
        log_block_size = u32(data, sb_off + 24)
        block_size = 1024 << log_block_size
        inode_size = u16(data, sb_off + 88)
        feat_incompat = u32(data, sb_off + 96)
        assert feat_incompat & 0x80 == 0, "64bit 피처가 켜진 이미지는 이 스크립트가 다루지 않는다"

        gd_off = (first_data_block + 1) * block_size
        bg_block_bitmap = u32(data, gd_off + 0)
        bg_inode_bitmap = u32(data, gd_off + 4)
        bg_inode_table = u32(data, gd_off + 8)
        bg_free_blocks = u16(data, gd_off + 12)
        bg_free_inodes = u16(data, gd_off + 14)

        # 루트(아이노드 2)의 extent에서 디렉터리 데이터 블록을 찾는다 —
        # eh_depth==0(단일 레벨)만 지지한다(ADR-129 v1 범위, 작은 테스트
        # 디렉터리는 항상 이 형태).
        root_off = bg_inode_table * block_size + (2 - 1) * inode_size
        eh_magic = u16(data, root_off + 40)
        assert eh_magic == 0xF30A, "루트 아이노드가 extent 기반이 아니다(ADR-129 v1 범위 밖)"
        eh_depth = u16(data, root_off + 40 + 6)
        assert eh_depth == 0, "루트 디렉터리 extent 트리가 다단계다(ADR-129 v1 범위 밖)"
        ee_block = u32(data, root_off + 52)
        assert ee_block == 0
        ee_len = u16(data, root_off + 56)
        ee_start_lo = u32(data, root_off + 60)
        assert ee_len == 1, "루트 디렉터리가 여러 블록이다(이 스크립트는 1블록만 다룬다)"
        root_data_block = ee_start_lo

        # 루트 디렉터리 블록에서 마지막 엔트리를 찾아 rec_len을 줄이고
        # 그 뒤에 새 엔트리를 추가한다.
        blk_off = root_data_block * block_size
        pos = 0
        last_off = None
        while pos < block_size:
            rec_len = u16(data, blk_off + pos + 4)
            if rec_len == 0:
                break
            last_off = pos
            pos += rec_len
        assert last_off is not None, "루트 디렉터리가 비어 있다(., ..조차 없음) — 손상된 이미지"

        last_inode = u32(data, blk_off + last_off)
        last_name_len = data[blk_off + last_off + 6]
        minimal_len = (8 + last_name_len + 3) & ~3
        old_rec_len = u16(data, blk_off + last_off + 4)
        new_entry_off = last_off + minimal_len
        new_rec_len = block_size - new_entry_off
        assert new_rec_len >= 8 + len(file_name), "루트 디렉터리 블록에 새 파일을 넣을 공간이 없다"

        set_u16(data, blk_off + last_off + 4, minimal_len)  # 마지막 기존 엔트리를 최소 크기로.

        # 새 파일용 아이노드 할당 — 아이노드 비트맵에서 EXT4_FIRST_INO(11)
        # 이후 첫 빈 비트를 고른다.
        ibm_off = bg_inode_bitmap * block_size
        new_inode_num = None
        for bit in range(11, u32(data, sb_off + 40)):  # s_inodes_per_group
            byte_idx = bit // 8
            bit_idx = bit % 8
            if not (data[ibm_off + byte_idx] & (1 << bit_idx)):
                data[ibm_off + byte_idx] |= 1 << bit_idx
                new_inode_num = bit + 1  # 아이노드 번호는 1부터.
                break
        assert new_inode_num is not None, "빈 아이노드가 없다"

        # 새 파일용 데이터 블록 할당(내용이 1블록을 넘으면 이 스크립트는
        # 다루지 않는다 — 테스트 픽스처용 작은 파일 전제).
        assert len(content) <= block_size, "이 스크립트는 1블록(=block_size바이트) 이하 파일만 지원한다"
        bbm_off = bg_block_bitmap * block_size
        blocks_per_group = u32(data, sb_off + 32)
        new_block_num = None
        for bit in range(blocks_per_group):
            byte_idx = bit // 8
            bit_idx = bit % 8
            if not (data[bbm_off + byte_idx] & (1 << bit_idx)):
                data[bbm_off + byte_idx] |= 1 << bit_idx
                new_block_num = first_data_block + bit
                break
        assert new_block_num is not None, "빈 블록이 없다"

        # 새 파일 내용을 쓴다(나머지는 0 패딩 — 이미지 전체가 이미
        # 0으로 초기화돼 있어 별도 처리 불필요).
        content_block_off = new_block_num * block_size
        data[content_block_off:content_block_off + len(content)] = content

        # 새 아이노드를 채운다: 일반 파일 + extent 1개(EXTENTS 플래그).
        new_inode_off = bg_inode_table * block_size + (new_inode_num - 1) * inode_size
        data[new_inode_off:new_inode_off + inode_size] = b"\x00" * inode_size
        set_u16(data, new_inode_off + 0, 0o100644)  # i_mode: S_IFREG|0644
        set_u32(data, new_inode_off + 4, len(content))  # i_size_lo
        set_u16(data, new_inode_off + 26, 1)  # i_links_count
        set_u32(data, new_inode_off + 28, (block_size // 512))  # i_blocks_lo(512바이트 섹터 수)
        set_u32(data, new_inode_off + 32, 0x80000)  # i_flags: EXT4_EXTENTS_FL

        ib_off = new_inode_off + 40
        set_u16(data, ib_off + 0, 0xF30A)  # eh_magic
        set_u16(data, ib_off + 2, 1)  # eh_entries
        set_u16(data, ib_off + 4, 4)  # eh_max
        set_u16(data, ib_off + 6, 0)  # eh_depth
        set_u32(data, ib_off + 8, 0)  # eh_generation
        set_u32(data, ib_off + 12, 0)  # ee_block
        set_u16(data, ib_off + 16, 1)  # ee_len
        set_u16(data, ib_off + 18, 0)  # ee_start_hi
        set_u32(data, ib_off + 20, new_block_num)  # ee_start_lo

        # 루트 디렉터리에 새 dirent를 추가한다(file_type=1: 일반 파일).
        name_bytes = file_name.encode("ascii")
        set_u32(data, blk_off + new_entry_off + 0, new_inode_num)
        set_u16(data, blk_off + new_entry_off + 4, new_rec_len)
        data[blk_off + new_entry_off + 6] = len(name_bytes)
        data[blk_off + new_entry_off + 7] = 1  # file_type: regular
        data[blk_off + new_entry_off + 8:blk_off + new_entry_off + 8 + len(name_bytes)] = name_bytes

        # 남은 카운터 갱신(슈퍼블록 + 그룹 디스크립터 둘 다) — 아이노드
        # 1개, 블록 1개 소비.
        set_u32(data, sb_off + 12, u32(data, sb_off + 12) - 1)  # s_free_blocks_count_lo
        set_u32(data, sb_off + 16, u32(data, sb_off + 16) - 1)  # s_free_inodes_count
        set_u16(data, gd_off + 12, bg_free_blocks - 1)  # bg_free_blocks_count_lo
        set_u16(data, gd_off + 14, bg_free_inodes - 1)  # bg_free_inodes_count_lo

        f.seek(0)
        f.write(data)

    print(f"ext4 이미지에 {file_name}({len(content)}바이트, inode={new_inode_num}, "
          f"block={new_block_num}) 주입 완료: {image_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
