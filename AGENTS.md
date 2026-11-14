# AGENTS.md

## このリポジトリについて

GitHubリポジトリ名: `tt-and-tk/pynesis`．

自作プログラミング言語Pynesis(`.pn`)のコンパイラのソースを置く．

## Pynthesisの中での位置

このリポジトリは，PYNQ-Z2(Zynq-7000)上に実装する自作CPUと，それを動かすソフトウェア群(アセンブラ・コンパイラ・OS)からなる自作パソコンのプロジェクトPynthesisの一部である．Pynthesisは以下の独立したGitHubリポジトリで構成され，各リポジトリは作業用のディレクトリ`pc/`の下に並べて置く．

| リポジトリ(GitHub) | ディレクトリ(`pc/`配下) | 役割 |
|:-|:-|:-|
| `specification` | `specification/` | Pynthesisの仕様書．各リポジトリのソースを変更するときの開発の手順も含む(唯一の一次情報源) |
| `pyntaxis` | `assembler/` | 自作アセンブリ言語Pyntaxis(`.pt`) → SystemVerilog ROM(`.sv`)，または自作OS Qosmosの実行ファイルへのアセンブラ |
| `pynesis`(このリポジトリ) | `compiler/` | 自作プログラミング言語Pynesis(`.pn`) → アセンブリ言語Pyntaxisへのコンパイラ．`pyntaxis`のソースファイルを取り込んで使用し，`.sv`または実行ファイルまで一貫変換も可能 |
| `qurge` | `mypc/` | CPU・メモリ・ROM等のハードウェア全体のVivadoプロジェクトと，ROM上で動く自作OS Qosmos(シェルやファイルシステムなど．Pynesisで記述) |
| `for-pynthesis-skills` | `for-pynthesis-skills/` | Pynthesisの各リポジトリで共有するissue起票・対応支援スキルとコーディング規約を提供する．特定のリポジトリが主担当と判断できない，全リポジトリに影響するissueの起票先でもある |

```
入力(.pn) → [pynesis(このリポジトリ)のコンパイラ] → アセンブリ(.pt) → [pyntaxisのアセンブラ] → SystemVerilog ROM(.sv) → [Vivado] → PYNQ-Z2上のハードウェア(qurge)
```

## 作業する前に読む文書

作業する前に，`specification`の次の文書を読む．

| 文書(`specification`の中のパス) | 内容 |
|:-|:-|
| `pynesis/develop/index.md` | コンパイラのソースの構成，ビルド，実装の決まり，テストの手順 |
| `README.md` | 仕様書全体の構成 |
