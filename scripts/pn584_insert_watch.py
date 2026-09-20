import gdb

# PN-584DB994 - pn584_alloc_watch.py가 call#29/#30(연속, 사이에 다른
# alloc/free 전혀 없음)에서 order=0 phys=0x7fc0000이 두 번 연달아
# 튀어나오는 걸 잡았고, pn584_freelist_dump.py는 PageFrameAllocator::
# init() 직후엔 중복이 전혀 없음을 확인했다 - 즉 버그는 런타임의
# 첫 29번 할당/분할(kObtainBlock의 재귀적 buddy 분할) 어딘가에서
# 생긴다. 가장 유력한 메커니즘: kInsertBlock(node, addr, order)이
# "이미 이 주소가 이 order의 free list 머리(head)인 상태"에서 다시
# 호출되면 `kAsBlock(addr)->next = head(=addr 자신)`가 되어 자기
# 자신을 가리키는 1-노드 순환 리스트가 만들어진다 - 그러면 그 뒤
# 몇 번을 pop해도 head가 절대 안 바뀌어(next가 항상 자기 자신) 매번
# 같은 주소가 나온다(정확히 관측된 현상과 일치). 이 스크립트는
# kInsertBlock 진입 시점마다 "이 addr이 이미 그 order의 현재 head인가"
# 를 직접 검사해, 그 순간 그대로 잡는다.

K_DIRECT_MAP_BASE = 0xFFFF800000000000


def log(msg):
    gdb.write(f"[pn584-insert] {msg}\n")
    gdb.flush()


class InsertWatch(gdb.Breakpoint):
    def stop(self):
        frame = gdb.selected_frame()
        # [주의, 실측으로 발견] gdb Python이 범용 레지스터를 부호 있는
        # 64비트로 돌려준다 - 커널 가상주소(최상위 비트 세팅됨)는 이때
        # 큰 음수가 되고, Python의 `format(negative, 'x')`는 2의 보수
        # 변환이 아니라 그냥 절댓값 앞에 '-'만 붙인다("-7f..." 같은
        # 잘못된 16진 문자열) - 반드시 0xFFFFFFFFFFFFFFFF로 마스킹해
        # unsigned로 되돌린 뒤에 포맷한다.
        node_ptr = int(frame.read_register("rdi")) & 0xFFFFFFFFFFFFFFFF  # Node& -> 포인터
        addr = int(frame.read_register("rsi")) & 0xFFFFFFFFFFFFFFFF
        order = int(frame.read_register("rdx")) & 0xFFFFFFFF
        # Node::freeListHeads[kMaxOrder+1]가 구조체 맨 앞 필드라 오프셋 0.
        head_virt = K_DIRECT_MAP_BASE if False else None
        # node_ptr는 gNodes[i]를 직접 가리키는 커널 가상주소(구조체 자체가
        # .bss에 있으므로 그 자체가 이미 가상주소) - 그대로 역참조한다.
        head_addr_expr = f"*(unsigned long*)(0x{node_ptr:x} + {order} * 8)"
        try:
            current_head = int(gdb.parse_and_eval(head_addr_expr))
        except gdb.error as e:
            log(f"경고: head 조회 실패({e}) - node_ptr=0x{node_ptr:x} order={order}")
            return False
        if current_head == addr:
            log(f"*** SELF-LOOP INSERT *** phys=0x{addr:x} order={order} - "
                f"이 order의 현재 head가 이미 이 주소다. 이대로 삽입하면 "
                f"이 블록의 next가 자기 자신을 가리키는 1-노드 순환이 생겨, "
                f"이후 몇 번을 pop해도 항상 같은 주소만 나오게 된다.")
            gdb.execute("bt")
            return True
        return False


InsertWatch("kInsertBlock", internal=False)
log("armed - watching kInsertBlock for self-loop (addr == current freeListHeads[order])")
