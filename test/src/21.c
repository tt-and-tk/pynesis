// sizeof演算子 (型名・変数名，コンパイル時定数)
char buf[8];
int len = sizeof(buf);      // グローバル初期化子(定数式): 8*1=8

void main(void) {
    int a = sizeof(int);    // 型名: 4
    int b = sizeof(char);   // 型名: 1
    int c = sizeof(short);  // 型名: 2
    int d = sizeof(buf);    // 変数名(配列): 8
    char x;
    int e = sizeof(x);      // 変数名(スカラー): 1
    int f = sizeof(LED);    // 変数名(書き込み専用ハードウェア変数): 4 (readableでなくてもsizeofは可能)
    int g = sizeof(RGBLED); // 同上: 4
}
