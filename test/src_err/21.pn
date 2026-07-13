// 意味解析エラー: sizeofの定数式コンテキストで配列パラメータ(サイズ不明)は不可 (異常系)
// (配列パラメータはサイズ情報を持たないため，case値にsizeofを使うとサイズ不明でエラーになる)
int f(int arr[]) {
    switch (arr[0]) {
        case sizeof(arr):
            return 1;
    }
    return 0;
}
void main(void) {
}
