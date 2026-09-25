#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "pn2asm.hpp"
#include "../assembler/asm2mc_main.hpp"

// ファイル名が指定の拡張子で終わっているか判定する(短い名前での範囲外アクセスを防ぐ)
static bool has_extension(const std::string &name, const std::string &ext) {
    return name.length() >= ext.length()
        && name.substr(name.length() - ext.length()) == ext;
}

// Qosmosの実行ファイル名として使えるかを返す
// アセンブラも出力の前に同じ規則で検査するが，その時点では中間アセンブリファイルが書き出し済みになるため，ここでも確かめる．
// Qosmosは拡張子(.)を持たないファイルだけを実行ファイルとして探し，名前は8.3形式の短い名前で扱うため，
// 基本名だけの1〜8文字で，FATの短い名前に使えない文字(制御文字・空白・記号)を含まないものに限る
static bool is_executable_name(const std::string &name) {
    // 基本名は1〜8文字
    if (name.empty() || name.length() > 8) return false;

    for (const unsigned char c : name) {
        // 制御文字・空白と，拡張子の区切り・短い名前に使えない記号
        if (c <= ' ' || c == 0x7f || strchr(".\"*+,/:;<=>?[\\]|", c) != nullptr) return false;
    }

    return true;
}

// コマンドライン引数を変換の前に検査する
// 2段階の変換はそれぞれ自前で引数を解析するが，2段階目で誤りが見つかった時点では1段階目が
// 中間アセンブリファイルを書き出し済みになるため，両段階の要求をここでまとめて確かめる．
// 引数が正しければtrue，誤りがあれば使い方を出力してfalseを返す．出力先が実行ファイルかどうかをis_binに返す
static bool check_args(int argc, char **argv, bool &is_bin) {
    std::string pn_file_name;   // 入力Pynesisソースファイル名
    std::string pt_file_name;   // 中間アセンブリファイル名
    std::string sv_file_name;   // 出力SystemVerilog ROMファイル名
    std::string bin_file_name;  // 出力する実行ファイルのファイル名
    bool has_bad_arg = false;   // 受け付けない引数があったか

    // 全ての引数でループ (コマンド名は飛ばす)
    for (int i = 1; i < argc; i++) {
        // 指定子なら，次の引数をそのパラメータとして取得する (各段階の解析と同じ解釈)
        if (argv[i][0] == '-') {
            std::string kind = argv[i];   // 指定を保存
            i++;

            // 値のない指定子は誤りとする
            if (i >= argc) {
                has_bad_arg = true;
                break;
            }

            // 指定されたパラメータを保存する (未知の指定子は誤りとする)
            if      (kind == "-pn") pn_file_name = argv[i];
            else if (kind == "-pt") pt_file_name = argv[i];
            else if (kind == "-sv") sv_file_name = argv[i];
            else if (kind == "-bin") bin_file_name = argv[i];
            else                    has_bad_arg = true;
        }
        // 指定子なしの引数は誤りとする
        else {
            has_bad_arg = true;
        }
    }

    is_bin = !bin_file_name.empty();
    // -svは省略を許し，指定された場合だけ拡張子を確かめる
    const bool sv_name_ok = sv_file_name.empty() || has_extension(sv_file_name, ".sv");
    // -binは指定された場合だけ，ディレクトリを除いたファイル名の部分を確かめる
    // (ディレクトリ部分の.(../binなど)は名前に関わらないため問わない)
    const bool bin_name_ok =
        !is_bin || is_executable_name(bin_file_name.substr(bin_file_name.find_last_of("/\\") + 1));
    // 出力先は-svと-binのどちらか一方だけを指定できる
    const bool both_output = is_bin && !sv_file_name.empty();

    // 受け付けない引数があるか，ファイル名が規則に合わないか，出力先を両方指定していれば，使い方を出力する
    if (has_bad_arg || !has_extension(pn_file_name, ".pn")
        || !has_extension(pt_file_name, ".pt") || !sv_name_ok || !bin_name_ok || both_output) {
        std::cout << "args fail" << std::endl
                  << "-pn: pynesis source file name. e.g. ~~.pn" << std::endl
                  << "    actual: " << pn_file_name << std::endl
                  << "-pt: intermediate asm file name. e.g. ~~.pt" << std::endl
                  << "    actual: " << pt_file_name << std::endl
                  << "-sv: output file name (optional). e.g. ~~.sv" << std::endl
                  << "    actual: " << sv_file_name << std::endl
                  << "-bin: executable file name (1-8 characters, no extension). e.g. HELLO (cannot be used with -sv)" << std::endl
                  << "    actual: " << bin_file_name << std::endl
                  << "arguments without a flag, unknown flags and flags without a value are not accepted" << std::endl;
        return false;
    }
    return true;
}

// メイン関数
// compile_pn_to_asm(Pynesisソース→アセンブリ)とassemble_asm_to_mc(アセンブリ→SystemVerilog ROMまたは実行ファイル)を
// 順に呼び出す入口．これが今後のコンパイラの入口となる(このプロジェクトのテスト対象はpn2asm.cppのままとする)．
//
// CLI: -pn(入力Pynesisファイル) -pt(中間アセンブリファイル) と，出力先として
// -sv(出力SystemVerilog ROMファイル)・-bin(出力する実行ファイル)のどちらか一方(どちらも省略可)．
// compile_pn_to_asmは-pn/-ptを，assemble_asm_to_mcは-pt/-sv/-binを見て，互いに関係ないフラグは無視するため，
// 引数の検査を通過した後は同じargv一式を両方へ渡す．
// ただし実行ファイルを出力する場合は，実行ファイル用のアセンブリを生成させるため，compile_pn_to_asmへ--bin-modeを足して渡す．
// (--bin-modeは値を取らないが，assemble_asm_to_mcは-で始まる引数の次を値として読むため，そちらへは渡さない)
// 処理に成功したら0，失敗したら1を返す
int main(int argc, char **argv) {
    bool is_bin = false;   // 実行ファイルを出力するか
    if (!check_args(argc, argv, is_bin)) {
        return 1;
    }

    // compile_pn_to_asmへ渡す引数 (実行ファイルを出力する場合は末尾に--bin-modeを足す)
    std::vector<char *> compile_argv(argv, argv + argc);
    char bin_mode_flag[] = "--bin-mode";   // compile_pn_to_asmへ足す指定子
    if (is_bin) compile_argv.push_back(bin_mode_flag);
    if (compile_pn_to_asm(static_cast<int>(compile_argv.size()), compile_argv.data()) != 0) {
        return 1;
    }
    if (assemble_asm_to_mc(argc, argv) != 0) {
        return 1;
    }
    return 0;
}
