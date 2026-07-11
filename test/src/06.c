// while ループ (break / continue，グローバル・ローカル変数)
int g = 0;
void main(void) {
    int i = 0;
    while (i < 5) {
        i += 1;
        if (i == 2) continue;   // 条件判定の先頭へ戻る
        if (i == 4) break;      // ループを脱出する
        g += i;
    }
}
