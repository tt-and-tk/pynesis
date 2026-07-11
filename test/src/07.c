// for ループ (break / continue，各部省略，グローバル・ローカル変数)
int g = 0;
void main(void) {
    int i;
    for (i = 0; i < 5; i += 1) {
        if (i == 2) continue;   // 更新部(i+=1)を経て条件へ
        if (i == 4) break;      // ループを脱出する
        g += i;
    }
    for (;;) {                  // 初期化・条件・更新を省略した無限ループ
        g += 1;
        if (g > 10) break;      // breakで脱出する
    }
}
