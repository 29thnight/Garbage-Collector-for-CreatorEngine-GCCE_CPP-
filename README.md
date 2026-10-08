# GCCE — CreatorEngine GC

CreatorEngine의 Scene, Entity, Component 메모리 수명을 관리하는 자체 GC 런타임이다. 설계와 도입 계획은 [docs/CreatorEngine_GC_Implementation_Plan.md](docs/CreatorEngine_GC_Implementation_Plan.md)에 있다.

현재 단계는 **M2 GameThread 증분 수집과 자동 요청**이다. 명시적 루트와 정확한 추적을 사용하는 nonmoving mark-and-sweep을 `collect_step(budget)`으로 프레임마다 나누어 진행하고, insertion barrier가 Mark 중의 참조 변경을 보완한다. M1의 동기 수집 `collect_full()`은 로딩과 종료 경계, 그리고 정확성 비교 기준으로 유지한다.

## 사용 예

```cpp
#include <gc/gc.hpp>

struct Component;

struct Entity
{
    std::vector<gc::trace_ref<Component>> components; // 강한 소유 간선
    gc::weak_ref<Entity> parent;                      // 관찰 역참조

    void gc_trace(gc::tracer& t) const { t.visit(components); }
};

struct Component
{
    gc::trace_ref<Entity> target; // 생존을 연장해야 하는 대상
    void gc_trace(gc::tracer& t) const { t.visit(target); }
};

gc::domain domain;                                   // 엔진 인스턴스당 하나
gc::root_ref<Entity> e = gc::make<Entity>(domain);   // 생성 스코프의 root
e->components.push_back(gc::make<Component>(domain));

gc::weak_ref<Entity> handle = e;
e = nullptr;

// 엔진 루프의 안전 구간에서 매 프레임
gc::step_result r = domain.collect_step({std::chrono::microseconds(250), 16});
```

## 공개 API

| 항목 | 역할 |
| --- | --- |
| `gc::domain` | 객체 등록소, 루트 목록, 수집기, 통계와 위반 진단 |
| `domain::collect_step(budget)` | 요청이나 최대 간격이 있을 때 사이클을 시작하고 soft budget만큼 진행 |
| `domain::collect_full()` | 진행 중 사이클을 끝낸 뒤 새 사이클을 완료까지 실행 |
| `domain::request_collection`, `set_allocation_threshold`, `set_max_cycle_interval` | 수명 이벤트, 할당량, 경과 시간에 따른 수집 요청 |
| `gc::make<T>(domain, args...)` | 생성 완료 후 게시하고 `root_ref<T>` 반환 |
| `gc::root_ref<T>` | 외부 루트. GameThread 전용이며 GC 객체 멤버로 쓰지 않는다 |
| `gc::trace_ref<T>` | GC 객체가 보유하는 강한 간선. `gc_trace`에서 방문해야 한다 |
| `gc::weak_ref<T>` | 비소유 참조. `lock()`은 GameThread에서만 하며 root_ref를 돌려준다 |
| `gc::tracer::visit` | trace_ref, `gc_trace`를 가진 값, optional, pair, 컨테이너를 재귀 방문 |
| `gc::lifecycle_of`, `gc::advance_lifecycle` | 논리적 수명 상태 조회와 한 단계 전진 |
| `gc::begin_cleanup_obligation` | 엔진 등록 시작 표시. destroyed 전에 도달 불가가 되면 사이클 중단 |

`gc::violation_kind`로 보고하는 위반은 잘못된 스레드의 변경, 중첩 수집, 추적 중 할당, 회수 확정 객체의 재게시, 정리 의무가 남은 객체의 도달성 상실, 종료 시 남은 root와 객체다. 기본 처리기는 메시지를 출력하고 중단한다.

## 빌드와 테스트

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

| 옵션 | 기본값 | 의미 |
| --- | --- | --- |
| `GCCE_BUILD_SHARED` | OFF | 런타임을 DLL/공유 라이브러리로 빌드 |
| `GCCE_THREAD_CHECKS` | ON | owner 스레드 검사 활성화 |
| `GCCE_BUILD_TESTS` | ON | 테스트 빌드 |

C++20이 필요하다. CI는 MSVC Debug/Release × 정적/DLL, GCC ASan/UBSan, Clang 공유 빌드를 실행한다.

## 검증 범위 (계획 12장)

| 번호 | 상태 |
| --- | --- |
| C01, C02, C07, C09, C10 | 동기 수집기로 검증 |
| C03, C05, C06, C08 | 증분 수집의 slice 경계에서 결정적으로 재현해 검증 |
| C04 | 사이클 사이의 컨테이너 변경을 검증. 컨테이너 내부 분할은 하지 않으므로 slice 중 cursor 문제는 없다 |
| C12 | 동기 수집기는 독립 oracle과 비교, 증분 수집기는 무작위 변경과 slice 크기에서 오회수 없음과 안정화 후 회수를 검증 |
| C11 | 잘못된 스레드 변경 탐지만 검증. 작업 차용 보호는 M3 이후 |

증분 테스트는 barrier 제거, 판정 직전 표시 작업 재소진 제거, 할당 cutoff 제거, Mark 중 정리 의무 표시 제거의 네 가지 변형을 각각 탐지하는지 확인했다.

## M1과 M2에서 정한 기준안

- 객체 헤더와 객체를 한 블록에 둔다. 참조는 헤더와 타입 보정된 포인터를 함께 보관해 다중 상속의 기반 클래스 참조를 지원한다.
- weak 슬롯 세대는 32비트로 1부터 시작한다. 최댓값에 도달한 슬롯은 재사용하지 않고 퇴역한다.
- Gray 스택은 사이클 시작 시 생존 객체 수만큼 미리 확보하고, 슬롯 반환 목록도 미리 확보해 sweep 중에 할당하지 않는다.
- 정리 의무 위반이 있으면 사이클 전체를 중단한다. 계획 5장의 격리 정책은 아직 구현하지 않았다.
- 종료 시 root가 남아 있으면 보고 후 root를 분리하고 객체를 해제하지 않는다.
- mark는 epoch 비교로 판정해 사이클마다 전체 색 초기화를 하지 않는다. epoch가 wrap되면 한 번 전체를 초기화한다.
- root 목록은 사이클 시작 시 한 번에 스캔한다. 이후 추가되는 root는 등록 시 barrier가 표시하고, root 제거는 작업이 없다.
- Gray가 비면 슬롯을 나누어 훑으며 정리 의무가 남은 미표시 객체를 후보로 모은다. Mark 중 새로 정리 의무가 생긴 객체는 즉시 표시해 이번 사이클에서 놓치지 않게 하고, 도달 불가라면 다음 사이클에서 보고한다.
- 회수 판정은 한 step 안에서 남은 표시 작업 소진, 후보 재확인, 할당 cutoff 고정, Sweep 진입을 함께 수행한다. 이 시간은 `worst_finalize`로 따로 기록한다.
- 회수 후보는 미표시이면서 cutoff 이전에 할당된 객체다. Sweep 중 생성된 객체는 같은 슬롯을 재사용해도 후보가 아니며, 후보에 대한 weak 승격과 강한 참조 저장은 저장 공간이 남아 있어도 실패하거나 위반으로 보고된다.
- 작업 단위는 객체 하나의 trace, 소멸자 하나, 또는 슬롯 256개 스캔이다. 각 단위의 시간을 재서 최장 단위와 step 초과를 기록한다. 컨테이너 재할당에 따른 barrier 실행 수는 `barrier_stores_during_mark`로 관찰한다.
- 메모리 압력에 따른 예산 조정은 아직 없다. M0 측정 이후 `step_budget`을 정하는 정책으로 추가한다.
