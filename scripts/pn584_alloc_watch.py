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


# [신규] PN-584DB994 - "타이머 인터럽트가 kObtainBlock() 임계구역
# 도중 끼어드는가"를 직접 확인한다. 커널 자신의 gInterruptDepth는
# kernel:: 안의 익명 네임스페이스에 있어 gdb가 이름으로 못 찾는다
# (실측으로 확인 - kernel::gInterruptDepth/'deferred_destruction.cpp'::
# gInterruptDepth 둘 다 실패) - 대신 그 카운터를 직접 증감시키는
# extern "C" 함수(맹글링 없음, 이름으로 확실히 찾음) 진입/이탈을
# 우리가 직접 세어 같은 값을 얻는다.
interrupt_depth = {"n": 0}


class EnterDepthWatch(gdb.Breakpoint):
    def stop(self):
        interrupt_depth["n"] += 1
        return False


class LeaveDepthWatch(gdb.Breakpoint):
    def stop(self):
        interrupt_depth["n"] -= 1
        return False


def current_interrupt_depth():
    return interrupt_depth["n"]


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
        depth = current_interrupt_depth()
        if addr in outstanding:
            prev_order, prev_idx, prev_depth = outstanding[addr]
            hits["double_alloc"] += 1
            log(f"*** DOUBLE ALLOC *** phys=0x{addr:x} order={self.order} "
                f"call#{idx}(interrupt_depth={depth}) - 이미 "
                f"call#{prev_idx}(order={prev_order}, interrupt_depth={prev_depth})에서 "
                f"할당된 채로 freeOrder() 없이 다시 할당됨 - "
                f"{'이번 호출은 인터럽트 컨텍스트 안!' if depth > 0 else '이번 호출은 인터럽트 컨텍스트 아님'}, "
                f"{'첫 호출도 인터럽트 컨텍스트 안!' if prev_depth > 0 else '첫 호출도 인터럽트 컨텍스트 아님'}")
            gdb.execute("bt")
            dump_freelist_state(self.order, addr)
            return True  # 여기서 실제로 멈춘다 - 결정적 증거
        outstanding[addr] = (self.order, idx, depth)
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
        prev_order, prev_idx, _prev_depth = outstanding.pop(addr)
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
class KPanicWatch(gdb.Breakpoint):
    # [신규] PN-584DB994의 기존(잘 알려진) #GP 크래시 자체도 이
    # 계측 세션 안에서 자주 잡힌다(이중할당보다도 먼저 걸리는 경우가
    # 대부분 - 계측 부하가 두 증상 모두를 더 자주 만들어낸다는 정황).
    # kIsrHandler가 vector 13(#GP)을 명시적으로 처리하지 않고
    # catch-all(kPanic)로 떨어뜨리는 게 원래부터 의도된 동작이라(새
    # 버그 아님), 이 지점에서 실제로 궁금한 건 단 하나 -
    # "이 순간에도 인터럽트 컨텍스트 안인가"(중첩 인터럽트/재진입
    # 가능성). InterruptFrame(interrupt_frame.h)의 vector 필드는
    # rax~r15(15개, 각 8바이트) 다음 오프셋 120에 있다.
    #
    # [신규, 갱신21] #PF(vector=0xe) 변종에서 cr2(폴트 주소)를 함께
    # 찍어 물리 페이지 이중소유 가설과 직접 연결을 시도한다(갱신20이
    # 남긴 "다음 세션 우선순위" 2번) - cr2가 direct map 범위
    # (K_DIRECT_MAP_BASE 이상)에 있으면 그 물리주소가 지금 우리
    # `outstanding` 장부에 실제로 잡혀 있는(=정상적으로 할당된 채인)
    # 페이지인지 대조한다 - 있으면 "정상 할당된 페이지에서 폴트가
    # 났다"는 뜻이라 매핑 자체가 깨졌다는 정황(이중할당으로 그 물리
    # 프레임의 페이지테이블 엔트리가 다른 소유자에게 재사용되며
    # 깨졌을 가능성), 없으면 그냥 무관한 별개의 커널 포인터 버그일
    # 가능성이 커진다.
    def stop(self):
        frame = gdb.selected_frame()
        frame_ptr = int(frame.read_register("rdi")) & 0xFFFFFFFFFFFFFFFF
        depth = current_interrupt_depth()
        if frame_ptr == 0:
            log(f"kPanic 도달 - frame=NULL(읽기 실패), interrupt_depth={depth}")
            return True
        try:
            vector = int(gdb.parse_and_eval(f"*(unsigned long*)0x{frame_ptr + 120:x}"))
            error_code = int(gdb.parse_and_eval(f"*(unsigned long*)0x{frame_ptr + 128:x}"))
            # [신규, 갱신21] 실제 폴트가 난 원본 컨텍스트의 rip/cs -
            # InterruptFrame(interrupt_frame.h) 레이아웃 그대로
            # (rip=+136, cs=+144) - kPanic() 자신의 현재 gdb 프레임이
            # 아니라 "무엇을 실행하다가" 이 인터럽트가 걸렸는지 직접
            # 알아낸다(cr2만으로는 코드 페치 폴트인지 데이터 접근
            # 폴트인지 구분 못 함 - rip==cr2면 코드 페치, 아니면 데이터).
            fault_rip = int(gdb.parse_and_eval(f"*(unsigned long*)0x{frame_ptr + 136:x}"))
            fault_cs = int(gdb.parse_and_eval(f"*(unsigned long*)0x{frame_ptr + 144:x}"))
            fault_rsp = int(gdb.parse_and_eval(f"*(unsigned long*)0x{frame_ptr + 160:x}"))
            rip_sym = ""
            try:
                rip_sym = " [" + gdb.execute(f"info symbol 0x{fault_rip:x}", to_string=True).strip() + "]"
            except gdb.error:
                pass
            # [신규, 갱신22] PN-584DB994 갱신22의 "최우선 검증" -
            # onTick()/onForcedMigration()이 kContextSwitchFromISR() 전에
            # EOI를 먼저 보내 이 코어에 인터럽트를 재허용하므로, 아직
            # 전환 전인 현재 Task의 8KiB 고정 커널 스택 위에 중첩
            # 인터럽트 프레임이 계속 쌓일 수 있다는 가설을 직접 수치로
            # 검증한다 - fault_rsp(폴트 시점 실제 스택 포인터)를 그
            # Task의 커널 스택 바닥(kernelStackTop-kernelStackSize)과
            # 비교해 여유/오버플로 여부를 계산한다. gCurrentTask는
            # `namespace kernel { namespace {} }`(이름 있는 네임스페이스
            # 안에 중첩된 무명 네임스페이스)라 gdb가 이름으로 못 찾는
            # 기존 한계(갱신20 문서 참고)와 똑같아, 맹글링된 심볼
            # 이름을 직접 식별자로 참조하는 우회를 쓴다(nm으로 확인한
            # `_ZN6kernel12_GLOBAL__N_112gCurrentTaskE`) - 이 프로젝트의
            # 헌트가 전부 SMP1(코어 0)이므로 인덱스 0 고정.
            stack_note = ""
            try:
                task_ptr = int(gdb.parse_and_eval("_ZN6kernel12_GLOBAL__N_112gCurrentTaskE[0]"))
                if task_ptr == 0:
                    stack_note = " task=NULL(idle?)"
                else:
                    stack_top = int(gdb.parse_and_eval(f"*(unsigned long*)0x{task_ptr + 24:x}"))
                    stack_size = int(gdb.parse_and_eval(f"*(unsigned long*)0x{task_ptr + 16:x}"))
                    stack_bottom = stack_top - stack_size
                    headroom = fault_rsp - stack_bottom
                    # [수정, 갱신23 - 실측으로 발견] 첫 실측에서
                    # headroom>0인데도 fault_rsp가 stack_top보다 위(=이
                    # gCurrentTask가 가리키는 스택 범위 자체를 완전히
                    # 벗어남)인 경우를 headroom만 보고 "여유 있음"으로
                    # 오판할 뻔했다 - "바닥 아래로 넘침"과 "이 Task의
                    # 스택 범위 자체가 아님"은 서로 다른 이상 징후라
                    # 따로 표시한다. 후자는 gCurrentTask가 실제 폴트
                    # 시점 이후(중첩된 onTick()이 이미 next로 갱신)의
                    # 값을 가리켜, 우리가 지금 읽는 "current task"가
                    # 폴트 당시 진짜로 그 스택 위에서 돌던 Task가
                    # 아닐 수 있다는 뜻일 수 있다.
                    if fault_rsp > stack_top:
                        stack_note = (f" task=0x{task_ptr:x} kernelStack=[0x{stack_bottom:x}, 0x{stack_top:x}) "
                                      f"fault_rsp=0x{fault_rsp:x}(스택 top보다 {fault_rsp - stack_top}바이트 위) "
                                      f"*** fault_rsp가 이 Task의 스택 범위 밖 - gCurrentTask가 폴트 이후 "
                                      f"갱신됐거나(중첩 onTick이 이미 next로 바꿔치기) 전혀 다른 스택 위에서 "
                                      f"실행 중이었을 가능성 ***")
                    else:
                        stack_note = (f" task=0x{task_ptr:x} kernelStack=[0x{stack_bottom:x}, 0x{stack_top:x}) "
                                      f"fault_rsp=0x{fault_rsp:x} headroom={headroom} bytes"
                                      f"{' *** 스택 바닥 이미 넘음(오버플로) ***' if headroom < 0 else ''}")
            except gdb.error as e:
                stack_note = f" 스택 여유 계산 실패({e})"
            cr2_note = ""
            if vector == 0xE:
                try:
                    cr2 = int(frame.read_register("cr2")) & 0xFFFFFFFFFFFFFFFF
                    cr2_note = f" cr2=0x{cr2:x}"
                    if cr2 == fault_rip:
                        cr2_note += " (rip와 정확히 일치 - 코드 페치 폴트, 이 주소로 점프/리턴 시도)"
                    if cr2 >= K_DIRECT_MAP_BASE:
                        physAddr = cr2 - K_DIRECT_MAP_BASE
                        pageAddr = physAddr & ~0xFFF
                        if pageAddr in outstanding:
                            order, idx, allocDepth = outstanding[pageAddr]
                            cr2_note += (f" -> direct-map phys=0x{pageAddr:x}가 outstanding 장부에 "
                                         f"있음(call#{idx}, order={order}, alloc시 interrupt_depth={allocDepth}) - "
                                         f"정상 할당된 페이지에서 폴트남(매핑 손상 정황, 이중할당 가설과 부합)")
                        else:
                            cr2_note += (f" -> direct-map phys=0x{pageAddr:x}가 outstanding 장부에 없음 "
                                         f"(해제됐거나 애초에 이 계측 이후 할당된 적 없는 페이지)")
                    else:
                        # [신규, 갱신21] direct map 범위 밖이어도 상위
                        # 32비트가 0xffffffff인 정상 커널 가상주소를
                        # 잘라낸 것처럼 보이는 값인지 확인 - 64->32비트
                        # 절단 버그의 증거가 될 수 있다(위 kPanic 도달
                        # 로그의 rip_sym과 대조).
                        truncated_as_kernel = 0xFFFFFFFF00000000 | cr2
                        try:
                            sym = gdb.execute(f"info symbol 0x{truncated_as_kernel:x}", to_string=True).strip()
                            if sym and not sym.startswith("No symbol"):
                                cr2_note += (f" -> direct map 범위 밖이지만 0xffffffff{cr2:08x}로 "
                                             f"복원하면 [{sym}] - 64비트 커널 포인터가 상위 32비트를 "
                                             f"잃은 것처럼 보임(절단 버그 의심)")
                            else:
                                cr2_note += " -> direct map 범위 밖(유저 영역 또는 무관한 낮은 주소)"
                        except gdb.error:
                            cr2_note += " -> direct map 범위 밖(유저 영역 또는 무관한 낮은 주소)"
                except gdb.error as e:
                    cr2_note = f" cr2 읽기 실패({e})"
            log(f"kPanic 도달 - vector=0x{vector:x} error_code=0x{error_code:x} "
                f"fault_rip=0x{fault_rip:x}{rip_sym} fault_cs=0x{fault_cs:x} "
                f"interrupt_depth={depth}{cr2_note}{stack_note} "
                f"({'인터럽트 컨텍스트 안(중첩)!' if depth > 1 else '정상 깊이'})")
        except gdb.error as e:
            log(f"kPanic 도달 - frame=0x{frame_ptr:x} 필드 읽기 실패({e}), interrupt_depth={depth}")
        return True


AllocEntry("kernel::PageFrameAllocator::allocOrder", "rdi")
AllocEntry("kernel::PageFrameAllocator::allocOrderBelow", "rsi")
FreeEntry("kernel::PageFrameAllocator::freeOrder", internal=False)
EnterDepthWatch("kEnterInterruptDepth", internal=False)
LeaveDepthWatch("kLeaveInterruptDepth", internal=False)
KPanicWatch("kPanic", internal=False)
log("armed - watching kernel::PageFrameAllocator alloc*/freeOrder for double-alloc/double-free "
    "+ kEnter/LeaveInterruptDepth for interrupt-context detection + kPanic for depth-at-crash")
