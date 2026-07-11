#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "c2asm.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "analyzer.hpp"
#include "generator.hpp"

// コマンドライン引数情報
typedef struct {
    std::string c_file_name;    // 入力Cソースファイル名
    std::string asm_file_name;  // 出力アセンブリファイル名
} args_t;

// 前宣言
static void get_args(int argc, char **argv, args_t &args);     // コマンドライン引数を取得する

// メイン関数: compile_c_to_asmをそのまま呼ぶだけ
// c2bin.cppに直接組み込むビルド(C2ASM_NO_MAIN定義時)ではmain多重定義を避けるため除外する
#ifndef C2ASM_NO_MAIN
int main(int argc, char **argv) {
    return compile_c_to_asm(argc, argv);
}
#endif

// Cソースをアセンブリに変換する本処理
// 処理に成功したら0，失敗したら1を返す
int compile_c_to_asm(int argc, char **argv) {
    args_t args;                                // コマンドライン引数
    std::vector<token_t> tokens;                // トークン列 (字句解析結果)
    node_t *ast = nullptr;                      // AST (構文解析結果)
    std::map<std::string, const symbol_t *> symbols;    // シンボルテーブル (意味解析結果)

    // コマンドライン引数を取得する
    get_args(argc, argv, args);
    if (args.c_file_name.empty() || args.asm_file_name.empty()) {
        return 1;
    }

    // Cソースファイルをまとめて読み込む
    std::ifstream c_file(args.c_file_name);
    if (!c_file) {
        std::cout << "cannot open c file: " << args.c_file_name << std::endl;
        return 1;
    }
    std::ostringstream ss;
    ss << c_file.rdbuf();
    const std::string src = ss.str();
    c_file.close();

    // 出力アセンブリファイルを開く
    std::ofstream asm_file(args.asm_file_name);
    if (!asm_file) {
        std::cout << "cannot open asm file: " << args.asm_file_name << std::endl;
        return 1;
    }

    try {
        // 字句解析を行い，トークン列を生成する (Lexer)
        lex(src, tokens);

        // 構文解析を行い，ASTを生成する (Parser)
        Parser parser(tokens);
        ast = parser();

        // 意味解析を行い，シンボルテーブルを構築する (Semantic Analyzer)
        Analyzer analyzer(ast);
        symbols = analyzer();

        // アセンブリコードを生成する (Code Generator)
        Generator generator(ast, symbols, analyzer.func_params(), analyzer.scratch_base(), asm_file);
        generator();

        asm_file.flush();
        asm_file.close();

        // 出力命令数がROMの上限(MAX_INSTRUCTION_COUNT)を超えていないか確認する
        // (命令行は先頭が半角スペース．ラベル行・.global行は先頭にスペースを付けない規約で判定する．
        //  ただしコメント行(先頭の空白を除いた最初の文字が';')は命令行に含めない)
        std::ifstream check_file(args.asm_file_name);
        int instruction_count = 0;
        std::string line;
        while (std::getline(check_file, line)) {
            const size_t pos = line.find_first_not_of(' ');
            if (pos != std::string::npos && line[0] == ' ' && line[pos] != ';') {
                instruction_count++;
            }
        }
        check_file.close();
        if (instruction_count > MAX_INSTRUCTION_COUNT) {
            throw std::string("compiler error: instruction count (")
                  + std::to_string(instruction_count) + ") exceeds maximum ("
                  + std::to_string(MAX_INSTRUCTION_COUNT) + ")";
        }

        // 正常終了を報告する
        std::cout << "compiled: " << args.asm_file_name << std::endl;
    }
    catch (std::string msg) {
        std::cout << msg << std::endl;
        asm_file.close();
        return 1;
    }

    return 0;
}

// コマンドライン引数を取得する
// -c: 必須引数．入力Cソースファイル名．
// -a: 出力アセンブリファイル名．省略した場合，Cファイル名の拡張子を .asm に変更して使用．
// 何も指定せずに引数を置いた場合，入力Cソースファイル名と解釈される．
void get_args(int argc, char **argv, args_t &args) {
    // 全ての引数でループ (コマンド名は飛ばす)
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];    // 引数一つ

        // 指定子なら
        if (arg[0] == '-') {
            std::string kind = arg;   // 指定を保存

            // インクリメントして次のパラメータを取得する
            i++;
            if (i >= argc) break;

            // 指定されたパラメータを保存する
            if      (kind == "-c") args.c_file_name   = argv[i];
            else if (kind == "-a") args.asm_file_name = argv[i];
        }
        // 指定子なしの引数は入力ファイル名と解釈する
        else {
            args.c_file_name = argv[i];
        }
    }

    // 入力ファイル名が .c で終わっているか確認する (短い名前での範囲外アクセスを防ぐ)
    const bool c_name_ok =
        args.c_file_name.length() >= 2
        && args.c_file_name.substr(args.c_file_name.length() - 2) == ".c";

    // 出力ファイル名が省略されていたら，入力ファイル名の末尾の ".c" を ".asm" に変えて使う
    if (c_name_ok && args.asm_file_name.empty()) {
        args.asm_file_name =
            args.c_file_name.substr(0, args.c_file_name.length() - 2) + ".asm";
    }

    // 出力ファイル名が .asm で終わっているか確認する
    const bool asm_name_ok =
        args.asm_file_name.length() >= 4
        && args.asm_file_name.substr(args.asm_file_name.length() - 4) == ".asm";

    // コマンドライン引数が不正ではないことをチェックする
    if (!c_name_ok || !asm_name_ok) {
        // メッセージを出力する
        std::cout << "args fail" << std::endl
                  << "-c: c source file name. e.g. ~~.c" << std::endl
                  << "    actual: " << args.c_file_name << std::endl
                  << "-a: output asm file name. e.g. ~~.asm" << std::endl
                  << "    actual: " << args.asm_file_name << std::endl;

        // 後の処理でエラーになるよう，コマンドライン引数をクリアする
        args.c_file_name.clear();
        args.asm_file_name.clear();
    }
}
