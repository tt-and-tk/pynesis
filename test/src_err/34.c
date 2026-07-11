// 意味解析エラー: switch文でdefaultラベルが複数ある (異常系)
void main(void) {
    int x = 1;
    switch (x) {
        default:
            break;
        default:
            break;
    }
}
