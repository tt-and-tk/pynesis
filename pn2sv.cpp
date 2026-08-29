#include "pn2asm.hpp"
#include "../assembler/asm2sv_main.hpp"

// メイン関数
// compile_pn_to_asm(Pynesisソース→アセンブリ)とassemble_asm_to_sv(アセンブリ→SystemVerilog ROM)を順に呼び出す入口．
// これが今後のコンパイラの入口となる(このプロジェクトのテスト対象はpn2asm.cppのままとする)．
//
// CLI: -pn(入力Pynesisファイル) -pt(中間アセンブリファイル) -sv(出力SystemVerilog ROMファイル) の3つ全てを指定する．
// compile_pn_to_asmは-pn/-ptを，assemble_asm_to_svは-pt/-svを見て，互いに関係ないフラグは無視するため，
// 同じargv一式をそのまま両方へ渡すだけでよい．
// スモールスタートのため，省略時に内部でエラーになる形で構わないこととし，明示的な検証は行わない
// (-ptを省略すると，compile_pn_to_asmは.pnから自動導出して成功するが，assemble_asm_to_svは-ptを持たず失敗する)．
// 処理に成功したら0，失敗したら1を返す
int main(int argc, char **argv) {
    if (compile_pn_to_asm(argc, argv) != 0) {
        return 1;
    }
    if (assemble_asm_to_sv(argc, argv) != 0) {
        return 1;
    }
    return 0;
}
