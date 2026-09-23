#include <iostream>
#include <string>
#include "pn2asm.hpp"
#include "../assembler/asm2sv_main.hpp"

// ファイル名が指定の拡張子で終わっているか判定する(短い名前での範囲外アクセスを防ぐ)
static bool has_extension(const std::string &name, const std::string &ext) {
    return name.length() >= ext.length()
        && name.substr(name.length() - ext.length()) == ext;
}

// コマンドライン引数を変換の前に検査する
// 2段階の変換はそれぞれ自前で引数を解析するが，2段階目で誤りが見つかった時点では1段階目が
// 中間アセンブリファイルを書き出し済みになるため，両段階の要求をここでまとめて確かめる．
// -pn: 必須引数．入力Pynesisソースファイル名．
// -pt: 必須引数．中間アセンブリファイル名．
// -sv: 出力SystemVerilog ROMファイル名．省略した場合，2段階目が中間アセンブリファイル名の拡張子を変更して使う．
// 指定子なしの引数は，1段階目は入力Pynesisソースファイル名，2段階目は中間アセンブリファイル名と解釈が食い違うため受け付けない．
// 引数が正しければtrue，誤りがあれば使い方を出力してfalseを返す
static bool check_args(int argc, char **argv) {
    std::string pn_file_name;   // 入力Pynesisソースファイル名
    std::string pt_file_name;   // 中間アセンブリファイル名
    std::string sv_file_name;   // 出力SystemVerilog ROMファイル名
    bool has_bare_arg = false;  // 指定子なしの引数があったか

    // 全ての引数でループ (コマンド名は飛ばす)
    for (int i = 1; i < argc; i++) {
        // 指定子なら，次の引数をそのパラメータとして取得する (各段階の解析と同じ解釈)
        if (argv[i][0] == '-') {
            std::string kind = argv[i];   // 指定を保存
            i++;
            if (i >= argc) break;

            if      (kind == "-pn") pn_file_name = argv[i];
            else if (kind == "-pt") pt_file_name = argv[i];
            else if (kind == "-sv") sv_file_name = argv[i];
        }
        else {
            has_bare_arg = true;
        }
    }

    // -svは省略を許し，指定された場合だけ拡張子を確かめる
    const bool sv_name_ok = sv_file_name.empty() || has_extension(sv_file_name, ".sv");

    if (has_bare_arg || !has_extension(pn_file_name, ".pn")
        || !has_extension(pt_file_name, ".pt") || !sv_name_ok) {
        std::cout << "args fail" << std::endl
                  << "-pn: pynesis source file name. e.g. ~~.pn" << std::endl
                  << "    actual: " << pn_file_name << std::endl
                  << "-pt: intermediate asm file name. e.g. ~~.pt" << std::endl
                  << "    actual: " << pt_file_name << std::endl
                  << "-sv: output file name (optional). e.g. ~~.sv" << std::endl
                  << "    actual: " << sv_file_name << std::endl
                  << "arguments without a flag are not accepted" << std::endl;
        return false;
    }
    return true;
}

// メイン関数
// compile_pn_to_asm(Pynesisソース→アセンブリ)とassemble_asm_to_sv(アセンブリ→SystemVerilog ROM)を順に呼び出す入口．
// これが今後のコンパイラの入口となる(このプロジェクトのテスト対象はpn2asm.cppのままとする)．
//
// CLI: -pn(入力Pynesisファイル) -pt(中間アセンブリファイル) -sv(出力SystemVerilog ROMファイル，省略可)．
// compile_pn_to_asmは-pn/-ptを，assemble_asm_to_svは-pt/-svを見て，互いに関係ないフラグは無視するため，
// 引数の検査を通過した後は同じargv一式をそのまま両方へ渡すだけでよい．
// 処理に成功したら0，失敗したら1を返す
int main(int argc, char **argv) {
    if (!check_args(argc, argv)) {
        return 1;
    }
    if (compile_pn_to_asm(argc, argv) != 0) {
        return 1;
    }
    if (assemble_asm_to_sv(argc, argv) != 0) {
        return 1;
    }
    return 0;
}
