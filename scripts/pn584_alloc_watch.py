import gdb

# PN-584DB994 갱신18이 제안한 다음 조사 - "물리 페이지 이중소유"
# 가설을 dr7 슬롯 관찰(pn584_watch.py, 결과: 외부 쓰기 자체를 못
# 잡음 - CPU 스토어가 아니라 물리 프레임 자체가 다른 소비자에게
# 넘어갔다는 정황 증거)에서 한 단계 더 근본으로 옮긴다: 아예
# `PageFrameAllocator::allocOrder()`/`freeOrder()` 호출 자체를 매번
# 가로채, 같은 물리주소가 "반납 없이 두 번 할당"되거나 "할당된 적
# 없는데 반납"되는 순간을 직접 잡는다 - 이게 실제로 잡히면
# PageFrameAllocator 자신의 buddy 장부 관리 버그(개별 호출자의
# "너무 이른 반납" 문제가 아니라)라는 뜻이라 조사 방향이 완전히
# 달라진다.
#
# System V AMD64: allocOrder(uint32_t order) - order는 edi, 반환은
# rax. freeOrder(uint64_t physAddr, uint32_t order) - physAddr는
# rdi, order는 esi. 심볼 이름은 DWARF 디버그 정보(-g, 항상 켜짐)로
# gdb가 직접 찾으므로 pn584_watch.py처럼 nm/objdump로 raw 주소를
# 미리 뽑아 둘 필요가 없다 - 훨씬 견고하다.

outstanding = {}  # physAddr -> (order, call_index) - 지금 할당된 채로 있는 프레임
call_index = {"n": 0}
hits = {"double_alloc": 0, "double_free": 0}
K_DIRECT_MAP_BASE = 0xFFFF800000000000  # paging.h의 kDirectMapBase(고정 constexpr).


def log(msg):
    gdb.write(f"[pn584-alloc] {msg}\n")
    gdb.flush()


def dump_freelist_state(order, focus_addr):
    # [신규] 이중할당이 잡힌 그 순간, node 0의 해당 order free list를
    # 직접 걸어 self-loop(같은 주소가 자기 자신을 next로 가리켜 pop이
    # 영원히 같은 주소만 내주는 상태)나 중복 항목이 지금 실제로
    # 존재하는지 그 자리에서 확인한다 - 별도 kInsertBlock 감시(무거워서
    # 부팅 진행 자체를 너무 늦춤)보다 훨씬 가볍다(이중할당이 실제로
    # 잡혔을 때만 도는 코드라 매 호출 오버헤드가 없다).
    try:
        node0 = gdb.parse_and_eval("'page_frame_allocator.cpp'::gNodes[0]")
    except gdb.error:
        node0 = gdb.parse_and_eval("gNodes[0]")
    head = int(node0["freeListHeads"][order]) & 0xFFFFFFFFFFFFFFFF
    log(f"  free list state (order={order}) 진입 시점 head=0x{head:x}")
    seen = []
    cur = head
    steps = 0
    while cur != 0 and steps < 20:
        seen.append(cur)
        virt = K_DIRECT_MAP_BASE + cur
        nxt = int(gdb.parse_and_eval(f"*(unsigned long*)0x{virt:x}")) & 0xFFFFFFFFFFFFFFFF
        log(f"    [{steps}] phys=0x{cur:x} -> next=0x{nxt:x}")
        if nxt == cur:
            log(f"    *** SELF-LOOP CONFIRMED *** phys=0x{cur:x}의 next가 자기 자신을 "
                f"가리킨다 - 이 노드를 pop해도 head가 절대 안 바뀐다(매번 같은 주소만 나옴)")
            break
        if nxt in seen:
            log(f"    *** CYCLE(자기 자신 아닌 더 긴 순환) 확인 *** next=0x{nxt:x}가 "
                f"이미 이 순회에서 지나간 주소")
            break
        cur = nxt
        steps += 1
    if focus_addr not in seen:
        log(f"  참고: 지금 문제된 주소 0x{focus_addr:x}는 현재 head에서부터 "
            f"{steps+1}단계 안에서 다시 보이지 않음(이미 pop되어 나갔을 수 있음)")


class AllocFinish(gdb.FinishBreakpoint):
    def __init__(self, order):
        super().__init__(gdb.newest_frame(), internal=False)
        self.order = order

    def stop(self):
        if not self.return_value:
            return False
        addr = int(self.return_value)
        if addr == 0:
            return False  # 할당 실패(OOM) - 추적 대상 아님
        call_index["n"] += 1
        idx = call_index["n"]
        if addr in outstanding:
            prev_order, prev_idx = outstanding[addr]
            hits["double_alloc"] += 1
            log(f"*** DOUBLE ALLOC *** phys=0x{addr:x} order={self.order} "
                f"call#{idx} - 이미 call#{prev_idx}(order={prev_order})에서 할당된 채로 "
                f"freeOrder() 없이 다시 할당됨")
            gdb.execute("bt")
            dump_freelist_state(self.order, addr)
            return True  # 여기서 실제로 멈춘다 - 결정적 증거
        outstanding[addr] = (self.order, idx)
        return False


class AllocEntry(gdb.Breakpoint):
    # order_reg: allocOrder(order)/allocOrderBelow(limit, order)는 order가
    # 두 번째 인자(esi)로 오는 함수도 섞여 있어(allocOrderBelow), 어느
    # 레지스터에서 읽을지 함수마다 다르게 지정한다 - 로깅 전용 값이라
    # 틀려도 이중할당/이중반납 판정 자체(physAddr 기준)에는 영향 없다.
    def __init__(self, symbol, order_reg):
        super().__init__(symbol, internal=False)
        self.order_reg = order_reg

    def stop(self):
        frame = gdb.selected_frame()
        order = int(frame.read_register(self.order_reg)) & 0xFFFFFFFF
        AllocFinish(order)
        return False


class FreeEntry(gdb.Breakpoint):
    def stop(self):
        frame = gdb.selected_frame()
        addr = int(frame.read_register("rdi"))
        order = int(frame.read_register("rsi")) & 0xFFFFFFFF
        call_index["n"] += 1
        idx = call_index["n"]
        if addr not in outstanding:
            hits["double_free"] += 1
            log(f"*** DOUBLE/UNKNOWN FREE *** phys=0x{addr:x} order={order} "
                f"call#{idx} - 현재 outstanding 목록에 없는 주소를 반납")
            gdb.execute("bt")
            return True  # 여기서도 결정적 증거로 멈춘다
        prev_order, prev_idx = outstanding.pop(addr)
        if prev_order != order:
            log(f"NOTE: order 불일치 phys=0x{addr:x} alloc_order={prev_order}(call#{prev_idx}) "
                f"free_order={order}(call#{idx}) - buddy 정책상 있을 수 있는 정상 케이스인지 "
                f"별도 확인 필요")
        return False


# [주의, 실측으로 발견한 계측 자체의 함정] allocOrderOnNode()는
# allocOrder() 안에서만 불린다(grep으로 전수 확인 - 외부 호출부
# 없음) - 이 둘을 동시에 감시하면 진짜 커널 버그가 아니라 "한 번의
# 실제 할당"이 내부 호출(allocOrderOnNode 반환)과 외부 호출
# (allocOrder 자신의 반환) 두 번 잡혀 outstanding에 두 번 등록되는
# 계측 자신의 거짓 양성이 난다(실제로 걸렸었음 - 최초 시도에서
# "DOUBLE ALLOC"이 뜬 원인이 바로 이것으로 확인됨, 진짜 버그가
# 아니었다). 그래서 allocOrderOnNode는 감시 목록에서 뺀다 -
# allocOrder만 감시해도 그 안에서 실제로 반환되는 주소는 그대로
# 잡힌다. allocOrderBelow는 allocOrder를 거치지 않는 완전히 독립된
# 구현(같은 파일 안에서 직접 free-list를 조작)이라 이중 계산 위험이
# 없다 - 계속 감시.
AllocEntry("kernel::PageFrameAllocator::allocOrder", "rdi")
AllocEntry("kernel::PageFrameAllocator::allocOrderBelow", "rsi")
FreeEntry("kernel::PageFrameAllocator::freeOrder", internal=False)
log("armed - watching kernel::PageFrameAllocator alloc*/freeOrder for double-alloc/double-free")
