// do-while ループ (break / continue，グローバル・ローカル変数)
int g = 0;
void main(void) {
    int i = 0;
    do {
        i += 1;
        if (i == 2) continue;   // 末尾の条件判定へ
        if (i == 4) break;      // ループを脱出する
        g += i;
    } while (i < 5);
}
