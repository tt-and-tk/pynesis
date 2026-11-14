#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "pn2asm.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "analyzer.hpp"
#include "generator.hpp"

// コマンドライン引数情報
typedef struct {
    std::string pn_file_name;   // 入力Pynesisソースファイル名
    std::string pt_file_name;   // 出力アセンブリファイル名
    bool is_bin_mode = false;   // Qosmosの実行ファイル用のアセンブリを出力するか (falseならROM用)
} args_t;

// 前宣言
static void get_args(int argc, char **argv, args_t &args);     // コマンドライン引数を取得する
// Pynesisソースをアセンブリに変換して返す(グローバル変数・文字列リテラルを指定した番地から置き，それらが占めるバイト数を3つ目の引数へ返す)
static std::string compile_to_asm(const args_t &args, int global_base, int &global_size);
static int count_instructions(const std::string &asm_text);    // アセンブリの命令数を数える

// メイン関数: compile_pn_to_asmをそのまま呼ぶだけ
// pn2mc.cppに直接組み込むビルド(PN2ASM_NO_MAIN定義時)ではmain多重定義を避けるため除外する
#ifndef PN2ASM_NO_MAIN
int main(int argc, char **argv) {
    return compile_pn_to_asm(argc, argv);
}
#endif

// Pynesisソースをアセンブリに変換する本処理
// 処理に成功したら0，失敗したら1を返す
int compile_pn_to_asm(int argc, char **argv) {
    args_t args;                                // コマンドライン引数

    // コマンドライン引数を取得する
    get_args(argc, argv, args);
    if (args.pn_file_name.empty() || args.pt_file_name.empty()) {
        return 1;
    }

    std::ofstream asm_file;                     // 出力アセンブリファイル

    try {
        std::string asm_text;  // 生成したアセンブリ
        int global_size;       // グローバル変数・文字列リテラルが占めるバイト数

        // グローバル変数を0番地から置いて生成し，命令数を数える(ROM用は，この生成結果をそのまま出力する)
        asm_text = compile_to_asm(args, 0, global_size);
        int instruction_count = count_instructions(asm_text);  // 生成した命令数
        // 実行ファイル用なら，グローバル変数を命令列の直後から置いて生成し直す
        // (命令数はコード生成を終えるまで分からないため，数えてから生成し直す．
        //  番地は常に32ビットの即値として埋め込むため，置き始める番地を変えても命令数は変わらない)
        if (args.is_bin_mode) {
            asm_text = compile_to_asm(args, instruction_count * INSTRUCTION_BYTES, global_size);
            // 置き始める番地を変えて命令数が変わった場合は，番地が命令列と重なるため内部エラーにする
            if (count_instructions(asm_text) != instruction_count) {
                throw std::string("compiler error: instruction count changed by placing global variables");
            }
        }

        // 実行ファイル用の場合 (命令列とグローバル変数は，シェルが割り振る領域の先頭から順に置くため，
        //  両者の合計が，実行ファイル用の範囲に収まる必要がある)
        if (args.is_bin_mode) {
            const int code_bytes = instruction_count * INSTRUCTION_BYTES;    // 命令列が占めるバイト数
            if (code_bytes + global_size > EXEC_AREA_SIZE) {
                throw std::string("compiler error: instructions (")
                      + std::to_string(code_bytes) + " bytes) and global variables ("
                      + std::to_string(global_size) + " bytes) exceed area for executable ("
                      + std::to_string(EXEC_AREA_SIZE) + " bytes)";
            }
        }
        // ROM用の場合
        else if (instruction_count > MAX_INSTRUCTION_COUNT) {
            throw std::string("compiler error: instruction count (")
                  + std::to_string(instruction_count) + ") exceeds maximum ("
                  + std::to_string(MAX_INSTRUCTION_COUNT) + ")";
        }

        // 出力アセンブリファイルを開き，生成したアセンブリを書き出す
        // (字句解析〜コード生成と大きさの検査でエラーになった場合にファイルを残さないよう，検査を終えてから開く)
        // (実行環境によらずLFで出力するよう，テキストモードによる改行コード変換(LF→CRLF)を避けてバイナリモードで開く)
        asm_file.open(args.pt_file_name, std::ios::binary);
        if (!asm_file) {
            throw std::string("compiler error: cannot open file '") + args.pt_file_name + "'";
        }
        asm_file << asm_text;
        asm_file.close();

        // 正常終了を報告する
        std::cout << "compiled: " << args.pt_file_name << std::endl;
    }
    catch (std::string msg) {
        std::cout << msg << std::endl;
        asm_file.close();
        return 1;
    }

    return 0;
}

// Pynesisソースをアセンブリに変換して返す
// グローバル変数・文字列リテラルは2つ目の引数の番地から上へ置き，それらが占めるバイト数を3つ目の引数へ返す
std::string compile_to_asm(const args_t &args, int global_base, int &global_size) {
    std::vector<token_t> tokens;    // トークン列 (字句解析結果)
    include_graph_t include_graph;  // ファイルごとの取り込み先 (字句解析結果)
    std::ostringstream asm_out;     // 生成したアセンブリの出力先

    // Pynesisソースファイル(取り込むファイルを含む)を読み込んで字句解析を行い，トークン列とファイルごとの取り込み先を生成する (Lexer)
    lex(args.pn_file_name, tokens, include_graph);

    // 構文解析を行い，ASTを生成する (Parser)
    Parser parser(tokens);
    node_t *ast = parser();    // AST (構文解析結果)

    // 意味解析を行い，シンボルテーブルを構築する (Semantic Analyzer)
    Analyzer analyzer(ast, include_graph, args.is_bin_mode, global_base);
    analyzer();

    // アセンブリコードを生成する (Code Generator)
    Generator generator(ast, analyzer.result(), asm_out);
    generator();

    global_size = analyzer.result().global_size;
    return asm_out.str();
}

// アセンブリの命令数を数える
// (コード生成は，命令行を半角空白で字下げし，ラベル行・.global行を字下げせずに出力する．
//  コメント行は命令行と同じく字下げするため，先頭の空白を除いた最初の文字が';'かどうかで見分ける)
int count_instructions(const std::string &asm_text) {
    std::istringstream lines(asm_text);  // 1行ずつ読み出す元
    std::string line;                    // 読み出した1行
    int instruction_count = 0;           // 数えた命令数

    // 1行ずつ，命令行かを調べて数える
    while (std::getline(lines, line)) {
        const size_t pos = line.find_first_not_of(' ');  // 先頭の空白を除いた最初の文字の位置
        // 命令行なら数える
        if (
            // 空白だけの行や空行でない
            pos != std::string::npos
            // かつ，字下げしている(ラベル行・.global行でない)
            && line[0] == ' '
            // かつ，コメント行でない
            && line[pos] != ';') {
            instruction_count++;
        }
    }
    return instruction_count;
}

// コマンドライン引数を取得する
// -pn: 必須引数．入力Pynesisソースファイル名．
// -pt: 出力アセンブリファイル名．省略した場合，Pynesisファイル名の拡張子を .pt に変更して使用．
// --bin-mode: 値を取らない．指定した場合，Qosmosの実行ファイル用のアセンブリを出力する．省略した場合はROM用．
// 何も指定せずに引数を置いた場合，入力Pynesisソースファイル名と解釈される．
void get_args(int argc, char **argv, args_t &args) {
    // 全ての引数でループ (コマンド名は飛ばす)
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];    // 引数一つ

        // 値を取らない指定子なら，指定されたことだけを覚える
        if (std::string(arg) == "--bin-mode") {
            args.is_bin_mode = true;
        }
        // 値を取る指定子なら
        else if (arg[0] == '-') {
            std::string kind = arg;   // 指定を保存

            // インクリメントして次のパラメータを取得する
            i++;
            if (i >= argc) break;

            // 指定されたパラメータを保存する
            if      (kind == "-pn") args.pn_file_name = argv[i];
            else if (kind == "-pt") args.pt_file_name = argv[i];
        }
        // 指定子なしの引数は入力ファイル名と解釈する
        else {
            args.pn_file_name = argv[i];
        }
    }

    // 入力ファイル名が .pn で終わっているか確認する (短い名前での範囲外アクセスを防ぐ)
    const bool pn_name_ok =
        args.pn_file_name.length() >= 3
        && args.pn_file_name.substr(args.pn_file_name.length() - 3) == ".pn";

    // 出力ファイル名が省略されていたら，入力ファイル名の末尾の ".pn" を ".pt" に変えて使う
    if (pn_name_ok && args.pt_file_name.empty()) {
        args.pt_file_name =
            args.pn_file_name.substr(0, args.pn_file_name.length() - 3) + ".pt";
    }

    // 出力ファイル名が .pt で終わっているか確認する
    const bool pt_name_ok =
        args.pt_file_name.length() >= 3
        && args.pt_file_name.substr(args.pt_file_name.length() - 3) == ".pt";

    // コマンドライン引数が不正ではないことをチェックする
    if (!pn_name_ok || !pt_name_ok) {
        // メッセージを出力する
        std::cout << "args fail" << std::endl
                  << "-pn: pynesis source file name. e.g. ~~.pn" << std::endl
                  << "    actual: " << args.pn_file_name << std::endl
                  << "-pt: output asm file name. e.g. ~~.pt" << std::endl
                  << "    actual: " << args.pt_file_name << std::endl
                  << "--bin-mode: output asm for a Qosmos executable (optional, takes no value)" << std::endl;

        // 後の処理でエラーになるよう，コマンドライン引数をクリアする
        args.pn_file_name.clear();
        args.pt_file_name.clear();
    }
}
