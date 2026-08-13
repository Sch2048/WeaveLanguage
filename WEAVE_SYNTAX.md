# Weave Language Syntax Specification v1.0

> Weave 是一种用于描述 Unreal Engine 蓝图图表的领域特定语言（DSL）。
> 它可以在文本与蓝图节点之间双向转换：生成（Blueprint → Weave）和还原（Weave → Blueprint）。

---

## 目录

1. [基本结构](#1-基本结构)
2. [关键字一览](#2-关键字一览)
3. [graphset — 蓝图声明](#3-graphset--蓝图声明)
4. [graph — 图表段落](#4-graph--图表段落)
5. [var — 变量声明](#5-var--变量声明)
6. [node — 节点声明](#6-node--节点声明)
7. [link — 连接声明](#7-link--连接声明)
8. [set — 设置默认值](#8-set--设置默认值)
9. [comment — 注释框](#9-comment--注释框)
10. [Schema ID 完整参考](#10-schema-id-完整参考)
11. [引脚命名规则](#11-引脚命名规则)
12. [变量类型参考](#12-变量类型参考)
13. [Tokenizer 规则](#13-tokenizer-规则)
14. [多图表模式](#14-多图表模式)
15. [常见错误与注意事项](#15-常见错误与注意事项)
16. [完整示例](#16-完整示例)

---

## 1. 基本结构

一个 Weave 脚本的基本结构如下：

```
graphset <BlueprintName> <BlueprintAssetPath>

var <VarName> : <Type>

graph <GraphName>

node <NodeId> : <SchemaId> @ (<X>, <Y>)
set <NodeId>.<PinName> = <Value>
link <FromNode>.<FromPin> -> <ToNode>.<ToPin>
comment "<Text>" @ (<X>, <Y>) size (<W>, <H>)
```

**行注释**使用 `#`：

```
# 这是一行注释
node a : call.KismetSystemLibrary.PrintString  # 行尾注释
```

---

## 2. 关键字一览

| 关键字 | 用途 | 必需 |
|--------|------|------|
| `graphset` | 声明目标蓝图名称和资产路径 | 可选 |
| `graph` | 开始一个图表段落 | **必需**（至少一个） |
| `var` | 声明蓝图变量（全局共享） | 可选 |
| `node` | 声明蓝图节点 | 可选 |
| `link` | 连接两个引脚 | 可选 |
| `set` | 设置引脚默认值或节点属性 | 可选 |
| `comment` | 创建注释框 | 可选 |

---

## 3. graphset — 蓝图声明

```
graphset <DisplayName> <AssetPath>
```

- `DisplayName`：蓝图的显示名称（一个 token）
- `AssetPath`：蓝图的完整资产路径

**示例：**
```
graphset BP_Player /Game/Blueprints/BP_Player.BP_Player
```

> **注意**：`graphset` 是可选的。如果省略，需要由调用方（Debugger 面板或 API）提供蓝图上下文。

---

## 4. graph — 图表段落

```
graph <GraphName>
```

- `GraphName`：目标图表的名称（一个 token）

**常见图表名：**
- `EventGraph` — 事件图表（最常用）
- `UserConstructionScript` — 构造脚本
- 任意自定义函数名，如 `MyFunction`、`Initialize`

**示例：**
```
graph EventGraph
```

> 如果目标蓝图中不存在该图表，且名称不包含 "EventGraph"，系统会自动创建同名函数图表。

---

## 5. var — 变量声明

```
var <VarName> : <Type>
```

变量声明是**全局的**，可以出现在任何 `graph` 之前或之间。变量会在目标蓝图中创建（如果尚不存在）。

### 基础类型

```
var MyBool : bool
var MyInt : int
var MyInt64 : int64
var MyFloat : float
var MyDouble : double
var MyString : string
var MyText : text
var MyName : name
var MyByte : byte
```

### 容器类型

使用 `array:`、`set:`、`map:` 前缀：

```
var MyArray : array:int
var MySet : set:string
var MyMap : map:string:int
```

> Map 类型格式：`map:<KeyType>:<ValueType>`

### 对象/结构体/枚举类型

```
var MyVector : Vector
var MyRotator : Rotator
var MyActor : AActor
var MyEnum : ECollisionChannel
var MyBPRef : /Game/Blueprints/BP_Enemy.BP_Enemy
var MyClassRef : class:AActor
```

> 详见 [变量类型参考](#12-变量类型参考)。

---

## 6. node — 节点声明

```
node <NodeId> : <SchemaId> @ (<X>, <Y>)
```

- `NodeId`：节点的唯一标识符（在 `link` 和 `set` 中引用）
- `SchemaId`：节点类型标识（详见 [Schema ID 参考](#10-schema-id-完整参考)）
- `@ (<X>, <Y>)`：节点在图表中的位置坐标

**特殊节点 ID：**
- `entry` — 函数图表的入口节点（自动映射到已有的 `FunctionEntry` 节点，不会新建）

**示例：**
```
node a : event.Actor.ReceiveBeginPlay @ (0, 0)
node b : call.KismetSystemLibrary.PrintString @ (300, 0)
node c : special.Branch @ (600, 0)
node entry : entry
```

> 如果 SchemaId 包含空格，需要用引号包裹：`node a : "call.My Class.My Function" @ (0, 0)`

---

## 7. link — 连接声明

```
link <FromNode>.<FromPin> -> <ToNode>.<ToPin>
```

- 从 `FromNode` 的**输出引脚** `FromPin` 连接到 `ToNode` 的**输入引脚** `ToPin`
- `->` 是连接方向符号

**引脚名包含空格时需加引号：**
```
link cast."As Pawn" -> target.self
```

### 执行链 vs 数据链

蓝图中有两种连接：

**执行链（白色线）**— 控制执行顺序：
```
link a.then -> b.execute       # a 执行完后执行 b
```

**数据链（彩色线）**— 传递数据：
```
link getHP.Health -> print.InString    # 将 Health 值传给 PrintString
```

### 重要规则

1. **执行输出引脚只能连接一个目标**。如需分支，使用 `special.Branch` 或 `special.Sequence`
2. **纯函数（如数学运算）没有执行引脚**，只需连接数据引脚即可自动计算
3. **禁止自连**：`FromNode` 和 `ToNode` 不能是同一个节点

---

## 8. set — 设置默认值

```
set <NodeId>.<PinName> = <Value>
```

设置节点引脚的默认值。

**引号规则：**
- 值包含空格、`.`、`=`、`(`、`)` 或与关键字同名时，**必须**用引号包裹
- 引号内的 `"` 用 `\"` 转义

```
set a.InString = "Hello World"
set a.Duration = "2.0"
set a.Count = 5
set a.bEnabled = true
```

### 特殊 set 语句

| 目标 | 用途 | 示例 |
|------|------|------|
| `nodeId.Expression` | MathExpression 表达式 | `set m.Expression = "A + B * 2"` |
| `nodeId.Class` | SpawnActor/ConstructObject 的类 | `set s.Class = class:BP_Enemy` |

### 向量和旋转的简写

```
set a.Location = vec(100,200,300)
set a.Rotation = rot(0,90,0)
```

---

## 9. comment — 注释框

```
comment "<Text>" @ (<X>, <Y>) size (<W>, <H>) [color (<R>, <G>, <B>, <A>)] [fontsize <N>]
```

- `"Text"` — 注释文本（支持 `\n` 换行、`\\` 反斜杠、`\"` 引号转义）
- `@ (<X>, <Y>)` — 位置坐标（整数）
- `size (<W>, <H>)` — 尺寸（整数）
- `color (<R>, <G>, <B>, <A>)` — 颜色，**0-255 整数**（可选，默认白色）
- `fontsize <N>` — 字号（可选，默认 18）

**示例：**
```
comment "Init Variables" @ (0, -100) size (400, 200) color (50, 150, 255, 255) fontsize 14
comment "Line1\nLine2" @ (500, 0) size (300, 150)
```

---

## 10. Schema ID 完整参考

Schema ID 是节点类型的唯一标识符，格式为点分隔的层级结构。

### 事件节点

```
event.<ClassName>.<EventName>
```

| 示例 | 说明 |
|------|------|
| `event.Actor.ReceiveBeginPlay` | BeginPlay 事件 |
| `event.Actor.ReceiveTick` | Tick 事件 |
| `event.Actor.ReceiveActorBeginOverlap` | 碰撞开始 |
| `event.Actor.UserConstructionScript` | 构造脚本 |

> ClassName 使用去掉 U/A 前缀后的名称（如 `Actor` 而非 `AActor`）。

### 函数调用

```
call.<ClassName>.<FunctionName>
```

| 示例 | 说明 |
|------|------|
| `call.KismetSystemLibrary.PrintString` | 打印字符串 |
| `call.KismetSystemLibrary.Delay` | 延迟 |
| `call.KismetMathLibrary.Add_IntInt` | 整数相加 |
| `call.KismetMathLibrary.Add_FloatFloat` | 浮点相加 |
| `call.KismetMathLibrary.Subtract_IntInt` | 整数相减 |
| `call.KismetMathLibrary.Multiply_FloatFloat` | 浮点相乘 |
| `call.KismetMathLibrary.GreaterEqual_FloatFloat` | 大于等于 |
| `call.KismetMathLibrary.LessEqual_IntInt` | 小于等于 |
| `call.KismetSystemLibrary.Conv_IntToString` | int 转 string |
| `call.GameplayStatics.GetPlayerController` | 获取玩家控制器 |
| `call.BP_MyActor_C.MyFunction` | 调用自定义蓝图函数 |

> **纯函数**（如数学运算、转换函数）没有 execute/then 引脚。

### 消息节点（接口调用）

```
message.<InterfaceClassName>.<FunctionName>
```

### 宏节点

```
macro.StandardMacros.<MacroName>        # 引擎内置宏
macro.<BlueprintPath>:<MacroName>       # 自定义宏
```

| 示例 | 说明 |
|------|------|
| `macro.StandardMacros.ForEachLoop` | ForEach 循环 |
| `macro.StandardMacros.IsValid` | 有效性检查 |

### 变量读取/写入

```
VariableGet.<ClassName>.<VarName>       # 读取变量
VariableSet.<ClassName>.<VarName>       # 写入变量
```

| 示例 | 说明 |
|------|------|
| `VariableGet.BP_Player_C.Health` | 读取 Health 变量 |
| `VariableSet.BP_Player_C.Health` | 写入 Health 变量 |

> Self 变量使用短类名（如 `BP_Player_C`），外部类变量使用完整路径名。

### 特殊节点

| Schema ID | 说明 |
|-----------|------|
| `entry` | 函数入口节点（映射到已有节点） |
| `special.Branch` | 条件分支（if/else） |
| `special.Sequence` | 执行序列 |
| `special.MathExpression` | 数学表达式 |
| `special.Make.<StructName>` | 构造结构体（如 `special.Make.FVector`） |
| `special.Break.<StructName>` | 拆解结构体（如 `special.Break.FRotator`） |
| `special.SpawnActorFromClass` | 生成 Actor |
| `special.ConstructObjectFromClass` | 构造对象 |
| `special.Cast.<TypePath>` | 类型转换（如 `special.Cast./Script/Engine.Pawn`） |
| `special.SwitchEnum.<EnumName>` | 枚举 Switch |
| `special.GetArrayItem` | 获取数组元素 |
| `special.Knot` | 重路由节点（Reroute） |

---

## 11. 引脚命名规则

### 通用执行引脚

| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |

### 各节点类型的引脚

**entry（函数入口）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输出 | Output | `then` |

**event 节点：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输出 | Output | `then` |
| 事件参数 | Output | 参数名（如 `DeltaSeconds`） |

**call 节点（非纯函数）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |
| 函数参数 | Input | 参数名（如 `InString`、`Duration`） |
| 返回值 | Output | `ReturnValue` |
| self 引用 | Input | `self` |

**call 节点（纯函数，如数学运算）：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 函数参数 | Input | 参数名（如 `A`、`B`） |
| 返回值 | Output | `ReturnValue` |

> **纯函数没有 execute/then 引脚！** 不要对纯函数使用执行链连接。

**special.Branch：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 条件 | Input | `Condition`（bool） |
| True 输出 | Output | `then` |
| False 输出 | Output | `else` |

**special.Sequence：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 序列输出 | Output | `then_0`、`then_1`、`then_2`... |

> 序列引脚会根据 link 引用自动创建。

**VariableGet：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 数据输出 | Output | **变量名本身**（如变量名为 `Health`，引脚就是 `Health`） |

**VariableSet：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 执行输出 | Output | `then` |
| 数据输入 | Input | **变量名本身**（如 `Health`） |
| 数据输出 | Output | `Output_Get` |

> **重要**：VariableSet 的输出引脚是 `Output_Get`，不是变量名！

**special.Cast：**
| 引脚 | 方向 | 名称 |
|------|------|------|
| 执行输入 | Input | `execute` |
| 对象输入 | Input | `Object` |
| 成功输出 | Output | `then` |
| 失败输出 | Output | `CastFailed` |
| 转换结果 | Output | `"As <ClassName>"`（含空格，需引号） |

---

## 12. 变量类型参考

### 基础类型

| Weave 类型 | UE 类型 |
|-----------|---------|
| `bool` | Boolean |
| `int` | Integer |
| `int64` | Integer64 |
| `float` | Float |
| `double` | Double |
| `string` | String |
| `text` | Text |
| `name` | Name |
| `byte` | Byte |

### 结构体类型（直接使用结构体名称）

| Weave 类型 | UE 类型 |
|-----------|---------|
| `Vector` | FVector |
| `Rotator` | FRotator |
| `Transform` | FTransform |
| `LinearColor` | FLinearColor |
| `Vector2D` | FVector2D |

### 对象类型（使用前缀 + 类名）

| Weave 类型 | UE 类型 |
|-----------|---------|
| `AActor` | Actor |
| `UStaticMeshComponent` | StaticMeshComponent |
| `/Game/BP/BP_Enemy.BP_Enemy` | 蓝图对象引用 |

### 类引用类型

```
class:AActor
class:/Game/BP/BP_Enemy.BP_Enemy
```

### 容器类型

```
array:<ElementType>     # 数组
set:<ElementType>       # 集合
map:<KeyType>:<ValueType>  # 映射
```

---

## 13. Tokenizer 规则

了解 Tokenizer 规则有助于理解为什么某些值需要引号包裹。

### 分隔符字符

以下字符会被拆分为独立 token：

| 字符 | 说明 |
|------|------|
| `:` | 冒号（类型分隔符） |
| `=` | 等号（set 赋值） |
| `.` | 点号（引脚访问） |
| `(` `)` | 括号（坐标） |
| `,` | 逗号（坐标分隔） |
| `@` | at 符号（位置标记） |

### 特殊规则

- `->` 被识别为单个 token（连接方向符号）
- `-` 后面不跟 `>` 时正常累积（如 `-100` 是单个 token）
- `#` 开始行注释（直到行尾）
- `"..."` 引号字符串作为单个 token（内部 `\"` 转义）
- 空白字符分隔 token

### 为什么浮点数需要引号

因为 `.` 是分隔符，`3.14` 会被拆分为 `3`、`.`、`14` 三个 token。因此在 `set` 语句中，浮点数值必须用引号：

```
set a.Duration = "2.0"     # 正确
set a.Duration = 2.0       # 错误！会被拆分
```

### 为什么含空格的值需要引号

空白字符也是分隔符，不加引号的值会被按空白拆分，可能被误认为后续关键字：

```
set a.InString = "Hello World"    # 正确
set a.InString = Hello World      # 错误！"World" 可能被当作下一个语句
```

---

## 14. 多图表模式

一个 Weave 脚本可以包含多个 `graph` 段落，用于在同一脚本中定义自定义函数及其调用。

### 语法

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var SharedVar : int                   # 变量是全局的

graph MyHelper                        # 第一个图表：自定义函数
node entry : entry
node a : call.KismetSystemLibrary.PrintString @ (300, 0)
link entry.then -> a.execute

graph EventGraph                      # 第二个图表：事件图表
node e : event.Actor.ReceiveBeginPlay
node f : call.BP_Test_C.MyHelper @ (300, 0)
link e.then -> f.execute
```

### 执行顺序

1. 按书写顺序依次处理每个 `graph` 段落
2. 每个段落处理后会编译蓝图（确保后续段落能引用新创建的函数）
3. **先定义被调用的函数，再定义调用方**

### 规则

- `var` 声明全局共享，可放在任何位置
- `node`、`link`、`set`、`comment` 属于当前 `graph` 段落
- 单 `graph` 脚本完全向后兼容，行为不变
- 不存在的函数图表会自动创建

---

## 15. 常见错误与注意事项

### 错误 1：对纯函数使用执行链

```
# 错误 ❌
node add : call.KismetMathLibrary.Add_IntInt @ (200, 0)
link a.then -> add.execute      # Add_IntInt 是纯函数，没有 execute 引脚！

# 正确 ✅
link a.then -> nextExecNode.execute    # 跳过纯函数，连接下一个有执行引脚的节点
link someOutput -> add.A               # 纯函数只需连数据引脚
link add.ReturnValue -> target.Input   # 自动在需要时计算
```

**纯函数列表（常见）**：所有 `KismetMathLibrary` 数学运算、比较运算、类型转换函数（`Conv_*`）、`KismetStringLibrary` 字符串操作等。

### 错误 2：执行输出连接多个目标

```
# 错误 ❌ — 执行输出只能连一个目标
link a.then -> b.execute
link a.then -> c.execute

# 正确 ✅ — 使用 Sequence 节点
node seq : special.Sequence @ (200, 0)
link a.then -> seq.execute
link seq.then_0 -> b.execute
link seq.then_1 -> c.execute
```

### 错误 3：VariableSet 输出引脚名

```
# 错误 ❌
link setHP.Health -> print.InString      # VariableSet 的输出不是变量名

# 正确 ✅
link setHP.Output_Get -> print.InString  # 输出引脚名是 Output_Get
```

### 错误 4：浮点数和含空格字符串未加引号

```
# 错误 ❌
set a.Duration = 2.0
set a.InString = Hello World

# 正确 ✅
set a.Duration = "2.0"
set a.InString = "Hello World"
```

### 错误 5：Link 方向反了

```
# 错误 ❌ — 从输入引脚链接到输出引脚
link subtract.A -> getVar.MyVar

# 正确 ✅ — 从输出引脚链接到输入引脚
link getVar.MyVar -> subtract.A
```

### 错误 6：多图表时函数定义顺序

```
# 错误 ❌ — 先调用后定义，编译时找不到函数
graph EventGraph
node f : call.BP_Test_C.MyFunc @ (300, 0)

graph MyFunc
node entry : entry

# 正确 ✅ — 先定义后调用
graph MyFunc
node entry : entry

graph EventGraph
node f : call.BP_Test_C.MyFunc @ (300, 0)
```

---

## 16. 完整示例

### 示例 1：简单的 BeginPlay 打印

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

node a : event.Actor.ReceiveBeginPlay @ (0, 0)
node b : call.KismetSystemLibrary.PrintString @ (300, 0)

set b.InString = "Hello Weave!"
set b.bPrintToScreen = true
set b.Duration = "2.0"

link a.then -> b.execute
```

### 示例 2：条件分支

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

var bReady : bool

node begin : event.Actor.ReceiveBeginPlay @ (0, 0)
node getReady : VariableGet.BP_Test_C.bReady @ (200, 96)
node branch : special.Branch @ (400, 0)
node printYes : call.KismetSystemLibrary.PrintString @ (700, -48)
node printNo : call.KismetSystemLibrary.PrintString @ (700, 96)

set printYes.InString = "Ready!"
set printNo.InString = "Not Ready"

link begin.then -> branch.execute
link getReady.bReady -> branch.Condition
link branch.then -> printYes.execute
link branch.else -> printNo.execute
```

### 示例 3：带自定义函数的多图表

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

var Counter : int

# 先定义函数
graph IncrementAndPrint
node entry : entry
node get : VariableGet.BP_Test_C.Counter @ (200, 0)
node add : call.KismetMathLibrary.Add_IntInt @ (400, 0)
node setVar : VariableSet.BP_Test_C.Counter @ (600, 0)
node conv : call.KismetSystemLibrary.Conv_IntToString @ (600, -80)
node print : call.KismetSystemLibrary.PrintString @ (800, 0)

set add.B = 1
set print.bPrintToScreen = true

link entry.then -> setVar.execute
link setVar.then -> print.execute
link get.Counter -> add.A
link add.ReturnValue -> setVar.Counter
link setVar.Output_Get -> conv.InInt
link conv.ReturnValue -> print.InString

# 再从 EventGraph 调用
graph EventGraph
node e : event.Actor.ReceiveBeginPlay @ (0, 0)
node call1 : call.BP_Test_C.IncrementAndPrint @ (300, 0)

link e.then -> call1.execute
```

### 示例 4：Tick 计时器 + Sequence

```
graphset BP_Test /Game/BP/BP_Test.BP_Test

graph EventGraph

var bActive : bool
var Timer : float

node tick : event.Actor.ReceiveTick @ (0, 0)
node getActive : VariableGet.BP_Test_C.bActive @ (0, 96)
node branch : special.Branch @ (200, 0)
node addTime : call.KismetMathLibrary.Add_FloatFloat @ (400, 96)
node getTimer : VariableGet.BP_Test_C.Timer @ (300, 192)
node setTimer : VariableSet.BP_Test_C.Timer @ (600, 0)
node cmp : call.KismetMathLibrary.GreaterEqual_FloatFloat @ (600, 128)
node branch2 : special.Branch @ (800, 0)
node resetTimer : VariableSet.BP_Test_C.Timer @ (1000, 0)
node seq : special.Sequence @ (1200, 0)
node printA : call.KismetSystemLibrary.PrintString @ (1400, 0)
node printB : call.KismetSystemLibrary.PrintString @ (1400, 128)

set cmp.B = "1.0"
set resetTimer.Timer = "0.0"
set printA.InString = "One second passed"
set printB.InString = "Also doing this"

# 执行链
link tick.then -> branch.execute
link getActive.bActive -> branch.Condition
link branch.then -> setTimer.execute
link setTimer.then -> branch2.execute
link branch2.then -> resetTimer.execute
link resetTimer.then -> seq.execute
link seq.then_0 -> printA.execute
link seq.then_1 -> printB.execute

# 数据链
link tick.DeltaSeconds -> addTime.A
link getTimer.Timer -> addTime.B
link addTime.ReturnValue -> setTimer.Timer
link setTimer.Output_Get -> cmp.A
link cmp.ReturnValue -> branch2.Condition
```

### 示例 5：注释框

```
comment "Init Section\nSetup all variables" @ (0, -200) size (600, 300) color (50, 150, 255, 255) fontsize 14
comment "Main Loop" @ (0, 200) size (1200, 400) color (255, 200, 50, 255) fontsize 16
```
