# GCCE — CreatorEngine GC

C++23 tracing GC 라이브러리. 명시적 루트와 정확한 추적을 사용하는 nonmoving mark-and-sweep이며, owner 스레드에서 프레임 예산만큼 나누어 수집한다. 설계 배경은 [docs/CreatorEngine_GC_Implementation_Plan.md](docs/CreatorEngine_GC_Implementation_Plan.md)에 있다.

엔진 연동(Scene, Entity, 스크립트 핸들 등)은 이 저장소의 범위가 아니다. 이 저장소는 엔진이 사용할 GC 런타임과 그 계약만 제공한다.

## 사용 예

```cpp
#include <gc/gc.hpp>

struct Item;

struct Inventory : gc::managed
{
    std::vector<gc::trace_ref<Item>> items; // 강한 간선
    gc::weak_ref<Inventory> parent;         // 관찰용 역참조

    void gc_trace(gc::tracer& t) const { t.visit(items); }
};

struct Item : gc::managed
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

## GC 타입과 생성·소멸 경로

GC 타입은 `gc::managed`를 상속하고 `gc::make`로만 생성한다. 소멸은 수집기만 한다. `gc::managed`는 클래스 범위의 할당 함수로 다른 경로를 컴파일 단계에서 막는다.

| 외부 코드 | 결과 |
| --- | --- |
| `new T`, `new T[n]`, `new (std::nothrow) T`, `new (p) T`, `std::make_unique<T>()` | 컴파일 오류 |
| `delete p`, `std::unique_ptr<T>` | 컴파일 오류 |
| `gc::make<T>`에서 `T`가 `gc::managed`를 상속하지 않음 | 컴파일 오류 |
| 클래스 자신의 멤버 함수 안의 `delete this` | 컴파일되지만 실행 시 메시지 후 중단 |
| 전역 한정 `::new (p) T` | 막지 않음 (의도적 우회) |

`operator delete`를 삭제하거나 private으로 두면 가상 소멸자를 가진 GC 타입이 컴파일되지 않으므로 protected로 둔다. `gc::managed`는 객체의 GC 정체성(포인터 하나)만 가진다. 정확히 한 번 상속해야 하며, 두 번 상속하면 `gc::make`가 컴파일 오류를 낸다. 스택, 전역, 다른 객체의 값 멤버로 GC 타입을 두는 것은 막지 않는다.

## 할당자

도메인마다 전용 블록 할당자를 둔다. 블록은 객체 헤더와 객체를 함께 담는다.

- **크기 클래스 풀 (기본):** 2 KiB 이하이고 정렬이 16 이하인 블록은 64 KiB 페이지를 24개 크기 클래스(16 B ~ 2 KiB)로 나누어 할당한다. 같은 클래스의 타입들은 페이지를 공유하고, 해제된 블록은 페이지의 free list로 재사용한다.
- **페이지 공급:** 페이지는 OS 가상 메모리(Linux `mmap`, Windows `VirtualAlloc`)의 2 MiB 청크에서 받고, 처음 쓸 때 commit한다. 빈 페이지는 어느 클래스든 다시 쓸 수 있다.
- **메모리 반환:** 빈 페이지는 바로 반환하지 않고 2사이클 동안 재사용을 기다린다. 그동안 다시 쓰이지 않으면 decommit해 물리 메모리를 OS에 돌려주고, 전부 빈 청크는 unmap한다.
- **개별 할당:** 2 KiB를 넘거나 정렬이 16보다 큰 블록은 전역 `operator new`로 할당한다.
- **디버그 지원:** ASan 빌드에서는 해제된 블록을 poison해 재사용 전 접근을 탐지하고, 일반 Debug 빌드에서는 0xDD로 채운다.
- **관찰:** `stats().heap_committed_bytes`, `heap_pages`, `heap_chunks`로 확보량을 본다. `heap_committed_bytes`는 commit된 풀 페이지와 개별 할당 블록의 바이트 합이며, 프로세스 RSS나 예약된 가상 주소 공간의 크기가 아니다.

`domain_config{heap_kind::system}`로 모든 블록을 전역 `operator new`로 보내는 비교용 힙을 고를 수 있다. 기본값은 `heap_kind::size_class_pools`다.

### 힙 비교 결과

`-DGCCE_BUILD_BENCHMARKS=ON`으로 `gcce_allocator_bench`(할당자 단독)와 `gcce_bench`(GC 전체)를 빌드한다. CI의 Release job도 같은 벤치마크를 실행해 로그에 남긴다.

| 시나리오 | Linux 크기 클래스 | Linux 시스템 | Windows 크기 클래스 | Windows 시스템 |
| --- | --- | --- | --- | --- |
| 할당자 단독: 할당 / 무작위 해제 / 재할당 / 전체 해제 (ns/op) | 9 / 93 / 67 / 22 | 68 / 264 / 308 / 176 | 8 / 49 / 63 / 16 | 96 / 140 / 138 / 107 |
| GC 할당 / 회수 (ns/객체) | 84 / 49 | 98 / 66 | 84 / 112 | 218 / 193 |
| Mark (ns/객체) | 96 | 107 | 204 | 220 |
| 프레임 churn p50 / p99 (ms) | 1.93 / 4.31 | 2.74 / 5.02 | 1.80 / 2.48 | 2.01 / 2.61 |
| 프레임 churn 종료 시 RSS | 100 MiB | 272 MiB | 49 MiB | 53 MiB |

Linux는 x86-64, GCC 14 Release, glibc malloc 기준이며 수집기 최적화(아래) 이후에 측정했다. Windows는 GitHub Actions `windows-latest`, MSVC Release, 기본 힙 기준이며, 절반 규모로 수집기 최적화 이전에 측정했다. 두 힙의 상대 비교는 같은 조건끼리만 의미가 있다. 이 수치는 기존 측정의 예시이며 이번 하드닝 변경 후 벤치마크를 다시 실행하지 않았다. 성능과 메모리 사용량은 워크로드, 플랫폼, 할당자와 실행 환경에 따라 달라진다.

### 수집기 최적화

Linux 벤치마크(절반 규모)에서 다음 변경의 효과를 측정했다.

| 변경 | 효과 |
| --- | --- |
| 작업 단위마다 시계를 읽지 않고, 단계 경계에서 시간을 재고 8단위 또는 무거운 단위 뒤에만 예산을 확인 | 회수 115 → 47 ns/객체, Mark 385 → 225 ns/객체 |
| Gray 스택과 추적 사이에 8칸 prefetch 링을 두고, 간선을 모아 prefetch한 뒤 표시 | Mark 225 → 약 80 ns/객체 (무작위 25만 객체 그래프) |
| `gc::make`의 root 등록에서 중복 검사 제거, Release에서 디버그 검사 끔 | 할당 100 → 84 ns/객체 |
| 결과 | 프레임 churn p50 2.2 → 1.0~1.5 ms, 같은 프레임 수에서 완료한 사이클 24 → 56 |

- **크기 클래스 풀:** 위 측정에서는 시스템 할당자보다 빠르거나 비슷했다. 특히 해제 후 재할당과, glibc에서 장시간 churn의 메모리 사용량에서 차이가 컸으며, 모든 워크로드에서의 우위를 보장하지 않는다.
- **타입별 풀(채택하지 않음):** 타입마다 고정 크기 풀을 두는 방식도 측정했다. 객체가 많은 타입에서는 크기 클래스 풀과 비슷했지만, 객체가 적은 타입이 많으면(200개 타입, 객체 6000개) 타입마다 페이지를 하나씩 commit해 크기 클래스 풀의 약 4배(12.7 MiB 대 3.2 MiB)를 썼다.
- **공통 한계:** 위 측정에서 객체가 무작위로 90% 해제되는 경우에는 어느 힙도 RSS가 줄지 않았다. 객체를 옮기지 않는 GC에서는 페이지마다 생존 객체가 남으면 메모리 반환이 제한된다.

## 참조 타입

| 타입 | 생존 | 사용 위치 |
| --- | --- | --- |
| `gc::root_ref<T>` | 대상과 그 그래프를 보존 | 스택, 전역, GC 밖의 컨테이너, 클로저. GC 객체 멤버로 쓰지 않는다 |
| `gc::trace_ref<T>` | 소유 객체가 살아 있을 때 대상 보존 | GC 객체의 멤버. `gc_trace`에서 방문해야 한다 |
| `gc::weak_ref<T>` | 보존하지 않음 | 관찰, 역참조, 캐시. `lock()`은 root_ref를 돌려준다 |

- 기반 클래스 변환(다중·가상 상속 포함), `static_ref_cast`, `dynamic_ref_cast`를 지원한다.
- `==`와 `std::hash`는 객체 정체성 기준이다. 정적 타입이 달라도 같은 객체면 같다.
- 모든 GC 객체는 멤버 함수 안에서 `root_from_this()`, `weak_from_this()`로 자기 참조를 얻는다. deducing this로 호출한 타입 그대로의 참조가 나오므로, 다중 상속 객체의 기반 클래스 멤버 함수에서는 그 기반 타입으로 보정된 참조가, const 객체에서는 `root_ref<const T>`가 나온다. 생성자 안, 복사본, `gc::make`로 만들지 않은 인스턴스에서는 빈 참조다.
- `root_ref::set_label("...")`로 루트 보유자 이름을 붙이면 진단에 표시된다.
- 같은 타입의 `weak_ref` 복사·대입은 `noexcept`이며 객체를 승격하지 않는다. 다른 타입으로의 변환(`const` 추가 포함)은 owner 스레드에서 `lock()`으로 생존을 확인한 뒤 포인터를 보정하며, 승격 실패 시 빈 참조가 된다. 이 변환은 `noexcept`가 아니며 Mark 중에는 일시적인 root 등록으로 표시·보존에 영향을 줄 수 있다.

## 추적

`gc_trace(gc::tracer&) const`에서 모든 강한 참조를 `t.visit(...)`로 방문한다. 직렬화 여부와 관계없이 모든 trace_ref를 방문해야 한다.

`visit`이 받는 타입은 `trace_ref`, `gc_trace`를 가진 값 타입, 그리고 이들을 담은 `std::optional`, `std::unique_ptr`, `std::variant`, `std::pair`, 모든 range 컨테이너(vector, deque, list, array, map, unordered_map, set 등)이다. 추적할 참조가 없는 타입을 넘기면 컴파일 오류가 난다. `weak_ref`는 방문 대상이 아니다.

추적 함수와 GC 객체의 소멸자는 할당, 수집, 게임 로직 호출을 하지 않는다. `gc_trace`는 예외를 던지지 않도록 작성하며, 소멸자와 위반 처리기는 예외를 던지면 안 된다. 소멸자에서 다른 GC 객체를 역참조하지 않는다.

## 수집

| API | 동작 |
| --- | --- |
| `collect_step(budget)` | 요청, 최대 간격, 메모리 압력이 있을 때 사이클을 시작하고 soft budget만큼 진행 |
| `collect_full()` | 진행 중 사이클을 끝낸 뒤 새 사이클을 완료까지 실행 |
| `request_collection()` | 요청만 기록. 실제 진행은 다음 step |
| `set_allocation_threshold(bytes)` | 지난 사이클 이후 할당량이 넘으면 요청 |
| `set_max_cycle_interval(d)` | 마지막 사이클 이후 시간이 지나면 시작 |
| `set_pacing({limit, start_fraction, max_scale})` | live 바이트가 한도의 비율을 넘으면 요청하고 step 예산을 최대 배율까지 늘림 |

- **증분 수집:** insertion barrier로 Mark 중의 참조 변경을 보완한다. 작업 단위는 객체 하나의 trace, 소멸자 하나, 최대 슬롯 256개 스캔이며, 시계는 보통 8단위의 배치 또는 무거운 단위 뒤에 확인한다. 시간 예산은 hard deadline이 아니다. 분할하지 않는 사이클 시작 작업(루트 스캔·용량 확보·epoch 초기화), 큰 trace나 긴 소멸자, 회수 판정, 사이클 종료 시 할당자 유지보수, 샘플링 간격과 `min_units` 때문에 예산을 넘을 수 있고, 그 초과는 통계에 기록된다.
- **회수 판정:** 남은 표시 작업 소진, 정리 의무 재확인, 할당 cutoff 고정, Sweep 진입을 한 step 안에서 수행한다. Sweep 중 생성된 객체는 회수 후보가 아니고, 후보는 저장 공간이 남아 있어도 weak 승격이 실패한다.
- **예외:** `collect_step()`·`collect_full()`에서 예외가 발생하면 진행 중 사이클을 중단으로 기록하고, 부분 표시 작업을 버리고 수집·추적 가드를 복구한 뒤 예외를 다시 던진다. 불완전한 표시 결과로 회수를 시작하지 않으며 idle 상태에서 새 사이클을 요청해 이후 다시 수집할 수 있다. `domain` 소멸자는 `noexcept`이므로 종료 수집에서 예외가 빠져나오면 `std::terminate`될 수 있다.

## 논리적 수명과 정리 의무

`lifecycle_state`(`alive → destroy_requested → destroying → destroyed`)는 GC 생존과 별개다. 전이는 한 단계씩 앞으로만 가며, 중복 요청은 `false`를 돌려준다.

`begin_cleanup_obligation(ref)` 이후 `destroyed`에 도달하기 전에 객체가 도달 불가가 되면 `unreachable_with_cleanup_obligation`을 보고하고 그 사이클을 중단한다.

| 정책 | 동작 |
| --- | --- |
| `obligation_policy::quarantine` (기본) | 위반 객체와 하위 그래프를 격리 루트로 보존. 다음 사이클부터 나머지 garbage는 정상 회수. `destroyed` 도달 시 격리 해제 |
| `obligation_policy::strict` | 위반이 해소될 때까지 매 사이클 중단 |

위 정책의 후속 동작은 위반 처리기가 반환할 때 적용된다. 기본 위반 처리기는 `abort`하므로, 프로세스 종료 없이 격리를 사용하려면 예외를 던지지 않고 반환하는 사용자 처리기를 `set_violation_handler`로 설정해야 한다.

## 진단과 통계

- `for_each_root`, `for_each_quarantined`: 타입, 라벨, 크기, 수명 상태 열거
- `gc::retention_path(ref)`: 루트(라벨 포함)부터 대상까지의 강한 경로. 도달 불가면 빈 결과
- `stats()`: 생존 객체와 바이트, 피크, 사이클 결과(표시 수, 간선 수, 회수 수, mark/sweep 시간), step 초과, 최장 샘플링 배치, 최장 회수 판정, barrier 실행 수, 격리 수
- `step_result::longest_batch`·`statistics::worst_batch`는 `collect_step`의 시계 확인 사이의 배치 시간이다. 회수 판정과 사이클 종료 유지보수가 포함될 수 있으며, 객체 하나의 trace나 소멸자 시간으로 해석하면 안 된다. 기존 `longest_unit`·`worst_unit`은 같은 값을 유지하는 호환 필드다.
- `step_result`·`collect_result`의 `startup_time`은 배치에서 분리한 사이클 시작 시간이며 사이클의 `mark_time`에 포함된다. `maintenance_time`은 종료 정리와 할당자 유지보수 시간으로 mark/sweep 시간과 별도다. 유지보수는 마지막 샘플링 배치와 step의 `elapsed`, 사이클의 `last_cycle_wall`에 포함된다.

## 위반 처리

`violation_kind`: 잘못된 스레드의 변경, 중첩 수집, 추적 중 할당, 회수 확정 객체의 재게시, 정리 의무 위반, 종료 시 남은 root와 객체. 기본 처리기는 메시지를 출력하고 `abort`한다. `set_violation_handler`로 바꿀 수 있다.

## 디버그 검사

`GC_DEBUG_CHECKS`가 켜지면(기본: Debug 빌드) 타입 시스템으로 막을 수 없는 잘못된 사용을 위반으로 보고한다. Release에서는 코드가 빠져 비용이 없다.

| 위반 | 탐지 방법 |
| --- | --- |
| `wrong_thread` | owner 스레드가 아닌 곳에서 root 등록, 참조 저장, 할당, 수집, weak 승격·만료 조회·다른 타입으로의 weak 변환 |
| `created_outside_make` | `gc::managed` 생성자가 `gc::make`의 생성 범위 밖에서 실행됨. 스택·전역 인스턴스와 복사본, GC 객체 안에 값으로 둔 GC 타입 |
| `root_inside_gc_object` | `root_ref`가 등록될 때 그 주소가 살아 있는 GC 객체 안에 있음 |
| `weak_ref_outlived_domain` | 파괴된 도메인의 `weak_ref`를 `lock()`·`expired()`하거나 다른 타입으로 변환함. 프로세스 전역 도메인 등록부로 확인하며, 처리기가 반환하면 해제된 메모리를 읽지 않고 빈 참조(`expired()`는 `true`)를 돌려준다 |

도메인에 속하지 않는 위반(도메인 밖의 `created_outside_make`, `weak_ref_outlived_domain`)은 `gc::set_global_violation_handler`로 받는다.

이 검사는 계약 위반 진단용이다. Release에서도 도메인은 그 도메인의 `weak_ref`보다 오래 살아야 하며, 도메인 파괴 후 `lock()`·`expired()`·다른 타입으로의 변환은 지원하지 않는다. Debug의 진단과 빈 참조 반환을 Release의 안전 보장으로 간주하면 안 된다.

## 작업 스레드에 객체 넘기기

`gc::pinned<T>`는 작업 스레드가 raw pointer로 객체를 쓰는 동안 그 객체를 보존하는 이동 전용 root다. wrapper 자체의 생성·이동·해제·소멸은 owner 스레드에서 한다. 작업 스레드에는 owner에서 얻은 raw pointer만 전달하고, 작업 완료를 동기화한 뒤 pin을 해제한다. pin은 메모리 수명만 보존하므로 객체 변경과의 동기화와 데이터 경쟁 방지는 별도로 필요하다. `for_each_root`와 retention path에 `gc.pinned` 라벨로 나타난다.

```cpp
gc::pinned<Mesh> pin(mesh);
jobs.run([p = pin.get()] { build_bvh(*p); });
jobs.wait();
pin.release();
```

## 모듈(DLL) 언로드

GC 객체는 자기 타입을 인스턴스화한 모듈의 코드(`gc_trace`, 소멸자)를 실행하므로, 그 모듈의 객체가 남아 있는 동안 모듈을 내리면 안 된다. `domain::objects_in_module(address)`는 주어진 주소가 속한 모듈에서 온 타입의 살아 있는 객체 수를 센다. 모듈은 타입 정보의 주소로 판별한다(Linux `dladdr`, Windows `GetModuleHandleEx`). 모듈의 객체를 놓고 `collect_full()`을 실행한 뒤 관련된 모든 도메인에서 유효한 모듈 주소로 개수가 0인지 확인한다. 이는 GC 객체 관점의 필요조건일 뿐 완전한 언로드 안전성 증명은 아니다. 실행 중인 스레드·작업, 등록된 콜백, 외부 참조, 타입 정보와 실행·할당·해제 코드의 DLL 소유권 등도 호출자가 별도로 정리·확인해야 한다. 주소의 모듈을 식별하지 못해도 0이 반환될 수 있다.

## 스레드 규약

root 등록·해제, 참조 저장, 할당, 수집, weak의 `lock()`·`expired()`와 다른 타입으로의 변환은 owner 스레드에서만 한다. 다른 스레드는 같은 타입의 `weak_ref`를 복사·보관할 수 있고, owner 스레드가 root로 보존하는 동안 객체를 읽을 수 있다. 이 허용은 다른 타입으로의 변환이나 동일한 참조 인스턴스의 동시 변경을 허용하지 않는다. 데이터 경쟁 방지는 사용자 책임이다. `bind_to_current_thread()`로 owner를 옮길 수 있으며, 이전 owner의 접근이 끝났음을 먼저 동기화해야 한다.

## 빌드와 테스트

C++23이 필요하다. explicit object parameter(deducing this)를 지원하는 GCC 14, Clang 18, MSVC 19.32 이상에서 빌드한다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

| 옵션 | 기본값 | 의미 |
| --- | --- | --- |
| `GCCE_BUILD_SHARED` | OFF | 런타임을 DLL/공유 라이브러리로 빌드 |
| `GCCE_DEBUG_CHECKS` | AUTO | owner 스레드 검사와 잘못된 사용 탐지. AUTO는 Debug에서 켜고 Release에서 끈다 |
| `GCCE_BUILD_TESTS` | ON | GoogleTest 테스트 빌드 |
| `GCCE_BUILD_BENCHMARKS` | OFF | 힙 비교 벤치마크 빌드 |

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
| `managed_test` | 모든 할당 형태의 컴파일 차단(정적 검사), 다형 타입 동작, `delete this` death test |
| `allocator_test` | 페이지 할당, 블록 재사용, 빈 페이지의 다른 크기 재사용, 2사이클 뒤 OS 반환, 큰 객체, 정렬, 주소 중복 없음, 두 힙에서 같은 작업의 내용 무결성 |
| `compile_fail/` | `new`, `new[]`, nothrow, placement, `delete`, `unique_ptr`, 비관리 타입 `make`가 빌드 실패하는지 확인. 각 경우마다 문제 줄만 뺀 대조 빌드가 성공해야 한다 |
| `misuse_test` | GC 타입의 스택·복사·값 멤버 생성, GC 객체 안의 root, 도메인보다 오래 산 weak_ref, 전역 처리기 death test |
| `pinned_test` | 작업 스레드가 읽는 동안의 보존, 이동 전용, 라벨 |
| `module_test` | 모듈별 객체 수. 공유 라이브러리 빌드에서는 플러그인 모듈을 실제로 로드해 객체 생성, 회수, 언로드까지 확인 |
| `edge_test` | mark epoch wrap과 슬롯 세대 최댓값의 퇴역을 테스트 훅으로 강제해 검증 |
| `stress_test` | seed 40개의 무작위 그래프. 전체 수집은 oracle과 정확히 일치, 증분 수집은 도달 가능 객체를 회수하지 않고 안정화 후 garbage를 남기지 않음 |

barrier 제거, 판정 직전 표시 작업 재소진 제거, 할당 cutoff 제거, Mark 중 정리 의무 표시 제거, 격리 루트 스캔 제거, epoch wrap 처리 제거, 퇴역 슬롯 재사용의 일곱 가지 구현 변형을 각각 테스트가 탐지하는지 확인했다.

CI는 MSVC Debug/Release × 정적/DLL, GCC Debug(ASan/UBSan), Clang Release(공유 라이브러리)에서 실행한다. 종료 위반 테스트의 의도적인 누수 때문에 `ASAN_OPTIONS=detect_leaks=0`을 사용하므로, 이 CI 설정은 누수 검출을 검증하지 않는다.
