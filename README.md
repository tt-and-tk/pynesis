# pynesis

このリポジトリは，自作プログラミング言語Pynesis(`.pn`)のコンパイラのソースを置く．コンパイラは，Pynesisで書いたプログラムを自作アセンブリ言語Pyntaxisへ変換する．アセンブラ`pyntaxis`のソースを取り込み，SystemVerilogのROM(`.sv`)または自作OS Qosmosの実行ファイルまで一貫して変換することもできる．

このリポジトリは，PYNQ-Z2(Zynq-7000)上に実装する自作CPUと，それを動かすソフトウェア群(アセンブラ・コンパイラ・OS)からなる自作パソコンのプロジェクトPynthesisの一部である．

## 文書

文書は，仕様書のリポジトリ`specification`に置く．このリポジトリにかかわる文書は次のとおり．

- [README.md](https://github.com/tt-and-tk/specification/blob/master/README.md): 仕様書全体の構成
- [terms.md](https://github.com/tt-and-tk/specification/blob/master/terms.md): Pynthesisの構成要素の名称と，Pynthesis全体で共通に使う用語
- [pynesis/](https://github.com/tt-and-tk/specification/tree/master/pynesis): Pynesisの言語仕様，コンパイラの使い方，コンパイラの開発の手順
