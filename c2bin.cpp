#include "c2asm.hpp"
#include "../assembler/asm2bin_main.hpp"

// メイン関数
// compile_c_to_asm(Cソース→アセンブリ)とassemble_asm_to_sv(アセンブリ→SystemVerilog ROM)を順に呼び出す入口．
// これが今後のコンパイラの入口となる(このプロジェクトのテスト対象はc2asm.cppのままとする)．
//
// CLI: -c(入力Cファイル) -a(中間アセンブリファイル) -b(出力SystemVerilog ROMファイル) の3つ全てを指定する．
// compile_c_to_asmは-c/-aを，assemble_asm_to_svは-a/-bを見て，互いに関係ないフラグは無視するため，
// 同じargv一式をそのまま両方へ渡すだけでよい．
// スモールスタートのため，省略時に内部でエラーになる形で構わないこととし，明示的な検証は行わない
// (-aを省略すると，compile_c_to_asmは.cから自動導出して成功するが，assemble_asm_to_svは-aを持たず失敗する)．
// 処理に成功したら0，失敗したら1を返す
int main(int argc, char **argv) {
    if (compile_c_to_asm(argc, argv) != 0) {
        return 1;
    }
    if (assemble_asm_to_sv(argc, argv) != 0) {
        return 1;
    }
    return 0;
}
