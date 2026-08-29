#pragma once

// Pynesisソースをアセンブリに変換する本処理．mainと同じ引数(argc, argv)を受け取る
// 処理に成功したら0，失敗したら1を返す
int compile_pn_to_asm(int argc, char **argv);
