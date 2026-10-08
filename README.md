# GCCE — CreatorEngine GC

CreatorEngine의 Scene, Entity, Component 메모리 수명을 관리하는 자체 GC 런타임이다. 설계와 도입 계획은 [docs/CreatorEngine_GC_Implementation_Plan.md](docs/CreatorEngine_GC_Implementation_Plan.md)에 있다.

현재 단계는 **M1 독립 GC 코어와 동기 기준 구현**이다. 명시적 루트와 정확한 추적을 사용하는 nonmoving mark-and-sweep을 `collect_full()` 하나로 수행한다. 참조 타입은 이미 모든 non-null 저장을 insertion barrier 경로로 보내므로, M2에서 Mark와 Sweep을 나눌 때 공개 계약은 바뀌지 않는다.

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

if (domain.collection_requested())
    domain.collect_full();                           // 엔진의 안전 구간에서만
```

## 공개 API

| 항목 | 역할 |
| --- | --- |
| `gc::domain` | 객체 등록소, 루트 목록, 수집기, 통계와 위반 진단 |
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

| 번호 | M1 상태 |
| --- | --- |
| C01, C02, C07, C09, C10 | 동기 수집기로 검증 |
| C04, C06, C08, C12 | 동기 부분 검증. 증분 경계는 M2 |
| C03, C05 | M2 (Mark 분할 이후 의미가 생김) |
| C11 | 잘못된 스레드 변경 탐지만 검증. 작업 차용 보호는 M3 이후 |

## M1에서 정한 기준안

- 객체 헤더와 객체를 한 블록에 둔다. 참조는 헤더와 타입 보정된 포인터를 함께 보관해 다중 상속의 기반 클래스 참조를 지원한다.
- weak 슬롯 세대는 32비트로 1부터 시작한다. 최댓값에 도달한 슬롯은 재사용하지 않고 퇴역한다.
- Gray 스택은 사이클 시작 시 생존 객체 수만큼 미리 확보하고, 슬롯 반환 목록도 미리 확보해 sweep 중에 할당하지 않는다.
- 정리 의무 위반이 있으면 사이클 전체를 중단한다. 계획 5장의 격리 정책은 아직 구현하지 않았다.
- 종료 시 root가 남아 있으면 보고 후 root를 분리하고 객체를 해제하지 않는다.
