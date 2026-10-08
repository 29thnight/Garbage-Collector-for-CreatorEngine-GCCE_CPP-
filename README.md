# GCCE — CreatorEngine GC

C++20 tracing GC 라이브러리. 명시적 루트와 정확한 추적을 사용하는 nonmoving mark-and-sweep이며, owner 스레드에서 프레임 예산만큼 나누어 수집한다. 설계 배경은 [docs/CreatorEngine_GC_Implementation_Plan.md](docs/CreatorEngine_GC_Implementation_Plan.md)에 있다.

엔진 연동(Scene, Entity, 스크립트 핸들 등)은 이 저장소의 범위가 아니다. 이 저장소는 엔진이 사용할 GC 런타임과 그 계약만 제공한다.

## 사용 예

```cpp
#include <gc/gc.hpp>

struct Item;

struct Inventory
{
    std::vector<gc::trace_ref<Item>> items; // 강한 간선
    gc::weak_ref<Inventory> parent;         // 관찰용 역참조

    void gc_trace(gc::tracer& t) const { t.visit(items); }
};

struct Item : gc::enable_ref_from_this<Item>
{
    gc::trace_ref<Inventory> owner;
    void gc_trace(gc::tracer& t) const { t.visit(owner); }
};

gc::domain domain;
gc::root_ref<Inventory> inv = gc::make<Inventory>(domain); // 외부 루트
inv->items.push_back(gc::make<Item>(domain));
inv->items.back()->owner = inv;                            // 순환도 회수된다

// 프레임마다 안전 구간에서
domain.collect_step({std::chrono::microseconds(250), 16});

// 로딩·종료 경계에서
domain.collect_full();
```

## 참조 타입

| 타입 | 생존 | 사용 위치 |
| --- | --- | --- |
| `gc::root_ref<T>` | 대상과 그 그래프를 보존 | 스택, 전역, GC 밖의 컨테이너, 클로저. GC 객체 멤버로 쓰지 않는다 |
| `gc::trace_ref<T>` | 소유 객체가 살아 있을 때 대상 보존 | GC 객체의 멤버. `gc_trace`에서 방문해야 한다 |
| `gc::weak_ref<T>` | 보존하지 않음 | 관찰, 역참조, 캐시. `lock()`은 root_ref를 돌려준다 |

- 기반 클래스 변환(다중·가상 상속 포함), `static_ref_cast`, `dynamic_ref_cast`를 지원한다.
- `==`와 `std::hash`는 객체 정체성 기준이다. 정적 타입이 달라도 같은 객체면 같다.
- `gc::enable_ref_from_this<T>`로 객체 안에서 `root_from_this()`, `weak_from_this()`를 얻는다. 생성자 안에서는 빈 참조를 돌려준다.
- `root_ref::set_label("...")`로 루트 보유자 이름을 붙이면 진단에 표시된다.

## 추적

`gc_trace(gc::tracer&) const`에서 모든 강한 참조를 `t.visit(...)`로 방문한다. 직렬화 여부와 관계없이 모든 trace_ref를 방문해야 한다.

`visit`이 받는 타입은 `trace_ref`, `gc_trace`를 가진 값 타입, 그리고 이들을 담은 `std::optional`, `std::unique_ptr`, `std::variant`, `std::pair`, 모든 range 컨테이너(vector, deque, list, array, map, unordered_map, set 등)이다. 추적할 참조가 없는 타입을 넘기면 컴파일 오류가 난다. `weak_ref`는 방문 대상이 아니다.

추적 함수와 GC 객체의 소멸자는 할당, 수집, 게임 로직 호출을 하지 않는다. 소멸자에서 다른 GC 객체를 역참조하지 않는다.

## 수집

| API | 동작 |
| --- | --- |
| `collect_step(budget)` | 요청, 최대 간격, 메모리 압력이 있을 때 사이클을 시작하고 soft budget만큼 진행 |
| `collect_full()` | 진행 중 사이클을 끝낸 뒤 새 사이클을 완료까지 실행 |
| `request_collection()` | 요청만 기록. 실제 진행은 다음 step |
| `set_allocation_threshold(bytes)` | 지난 사이클 이후 할당량이 넘으면 요청 |
| `set_max_cycle_interval(d)` | 마지막 사이클 이후 시간이 지나면 시작 |
| `set_pacing({limit, start_fraction, max_scale})` | live 바이트가 한도의 비율을 넘으면 요청하고 step 예산을 최대 배율까지 늘림 |

- **증분 수집:** insertion barrier로 Mark 중의 참조 변경을 보완한다. 작업 단위는 객체 하나의 trace, 소멸자 하나, 슬롯 256개 스캔이며 단위 경계에서 예산을 확인한다. 큰 컨테이너 하나나 긴 소멸자는 예산을 넘을 수 있고, 그 초과는 통계에 기록된다.
- **회수 판정:** 남은 표시 작업 소진, 정리 의무 재확인, 할당 cutoff 고정, Sweep 진입을 한 step 안에서 수행한다. Sweep 중 생성된 객체는 회수 후보가 아니고, 후보는 저장 공간이 남아 있어도 weak 승격이 실패한다.

## 논리적 수명과 정리 의무

`lifecycle_state`(`alive → destroy_requested → destroying → destroyed`)는 GC 생존과 별개다. 전이는 한 단계씩 앞으로만 가며, 중복 요청은 `false`를 돌려준다.

`begin_cleanup_obligation(ref)` 이후 `destroyed`에 도달하기 전에 객체가 도달 불가가 되면 `unreachable_with_cleanup_obligation`을 보고하고 그 사이클을 중단한다.

| 정책 | 동작 |
| --- | --- |
| `obligation_policy::quarantine` (기본) | 위반 객체와 하위 그래프를 격리 루트로 보존. 다음 사이클부터 나머지 garbage는 정상 회수. `destroyed` 도달 시 격리 해제 |
| `obligation_policy::strict` | 위반이 해소될 때까지 매 사이클 중단 |

## 진단과 통계

- `for_each_root`, `for_each_quarantined`: 타입, 라벨, 크기, 수명 상태 열거
- `gc::retention_path(ref)`: 루트(라벨 포함)부터 대상까지의 강한 경로. 도달 불가면 빈 결과
- `stats()`: 생존 객체와 바이트, 피크, 사이클 결과(표시 수, 간선 수, 회수 수, mark/sweep 시간), step 초과, 최장 작업 단위, 최장 회수 판정, barrier 실행 수, 격리 수

## 위반 처리

`violation_kind`: 잘못된 스레드의 변경, 중첩 수집, 추적 중 할당, 회수 확정 객체의 재게시, 정리 의무 위반, 종료 시 남은 root와 객체. 기본 처리기는 메시지를 출력하고 `abort`한다. `set_violation_handler`로 바꿀 수 있다.

## 스레드 규약

root 등록, 참조 저장, 할당, 수집, weak 승격은 owner 스레드에서만 한다. 다른 스레드는 weak_ref를 복사·보관할 수 있고, owner 스레드가 root로 보존하는 동안 객체를 읽을 수 있다. 데이터 경쟁 방지는 사용자 책임이다. `bind_to_current_thread()`로 owner를 옮길 수 있다.

## 빌드와 테스트

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

| 옵션 | 기본값 | 의미 |
| --- | --- | --- |
| `GCCE_BUILD_SHARED` | OFF | 런타임을 DLL/공유 라이브러리로 빌드 |
| `GCCE_THREAD_CHECKS` | ON | owner 스레드 검사 |
| `GCCE_BUILD_TESTS` | ON | GoogleTest 테스트 빌드 |

테스트는 GoogleTest를 사용한다. 설치된 패키지가 있으면 그것을 쓰고, 없으면 FetchContent로 받는다. 오프라인이면 `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<경로>`를 지정한다.

| 파일 | 내용 |
| --- | --- |
| `refs_test` | 참조 복사·이동·재배치, 라벨, 정체성 비교, hash, 캐스트, ref_from_this |
| `weak_test` | 만료, 승격, 슬롯 재사용 세대, 회수 중 승격 거부, 스레드 간 복사 |
| `collection_test` | 순환, 공유 노드, 20만 단계 리스트, 다형성, 가상 상속, 정렬, 일반 멤버 소멸 |
| `containers_test` | 표준 컨테이너·optional·variant·unique_ptr 추적, 컨테이너 변경과 알고리즘 |
| `construction_test` | 생성자 예외, 중첩 생성, 재진입·재게시 위반, 기본 처리기 death test |
| `lifecycle_test` | 상태 전이, 파괴된 객체의 하위 그래프, 격리·엄격 정책 |
| `incremental_test` | barrier, 판정 직전 승격, Sweep 중 생성, 예산 초과와 barrier 비용, 요청 조건 |
| `pacing_test` | 메모리 한도에 따른 요청과 예산 배율 |
| `diagnostics_test` | 루트 열거, retention path, 격리 목록, 시간 통계 |
| `threading_test` | owner 스레드 위반, owner 이동, 보존된 객체의 병렬 읽기, 독립 도메인 |
| `shutdown_test` | 종료 시 회수, 진행 중 사이클 종료, 남은 root와 정리 의무 |
| `scenarios_test` | 이중 연결 리스트, weak 부모를 가진 BST, 그래프, LRU 캐시, observer, 클로저 |
| `stress_test` | seed 40개의 무작위 그래프. 전체 수집은 oracle과 정확히 일치, 증분 수집은 도달 가능 객체를 회수하지 않고 안정화 후 garbage를 남기지 않음 |

barrier 제거, 판정 직전 표시 작업 재소진 제거, 할당 cutoff 제거, Mark 중 정리 의무 표시 제거, 격리 루트 스캔 제거의 다섯 가지 구현 변형을 각각 테스트가 탐지하는지 확인했다.

CI는 MSVC Debug/Release × 정적/DLL, GCC Debug(ASan/UBSan), Clang Release(공유 라이브러리)에서 실행한다.
