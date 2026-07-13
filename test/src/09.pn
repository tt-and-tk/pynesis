// switch (フォールスルー: case共有・breakなし，グローバル・ローカル変数)
int g = 0;
void main(void) {
    int x = 2;
    switch (x) {
        case 1:
        case 2:        // case 1,2 で g=10 を共有 (フォールスルー)
            g = 10;
            break;
        case 3:
            g = 20;    // breakなし → default へフォールスルー
        default:
            g = 30;
    }
}
