---
name: issue-resolve
description: GitHub issueに対応する。1issue=1回の実行で，複数リポジトリにまたがる修正もスキル内部で完結させる。「issue #Nに対応して」で起動。
---

GitHub issueへの対応 (調査・ブランチ作成・修正・PR作成) を行う．粒度は1issue=1回の実行．対応が複数リポジトリにまたがる場合も，このスキル内で完結させる．

## 手順

1. issue内容を取得する
   ```
   gh issue view <番号> --repo <owner>/<repo>
   ```
2. 原因調査を行い，影響範囲 (対象リポジトリ・対象ファイル) を特定する．issue本文中に推測が書かれている場合は，事実かどうかをここで検証する
3. 調査結果と修正方針をユーザーに提示し，承認を得る (実装前に必ず説明する)
4. 影響リポジトリごとに以下を行う
   1. デフォルトブランチを最新化する
      ```
      git checkout <デフォルトブランチ>
      git pull
      ```
   2. ブランチを作成する (命名: `fix/issue-<番号>-<内容を表す短い語句>`)
      ```
      git checkout -b fix/issue-<番号>-<内容を表す短い語句>
      ```
   3. 修正を行う (ユーザーの承認を得てから実施する)
   4. コミットしてpushし，Draft状態のPRを作成する
      ```
      git add <変更したファイル>
      git commit -m "<コミットメッセージ>"
      git push -u origin fix/issue-<番号>-<内容を表す短い語句>
      gh pr create --repo <owner>/<repo> --draft --title "<タイトル>" --body "<本文>"
      ```
   5. ソースレビューを行い，指摘があれば修正してコミット・pushする．レビューと修正は1回で終わるとは限らず，このコミット・push は複数回繰り返してよい
      ```
      claude -p --tools "Read,Grep,Glob" -- "コードレビューを依頼する文言" > review.md
      ```
      ```
      git add <変更したファイル>
      git commit -m "<コミットメッセージ>"
      git push
      ```
   6. ユーザーがPRにレビューコメントを追加した場合は取得して確認し，指摘があれば修正してコミット・pushする
      ```
      gh api repos/<owner>/<repo>/pulls/<PR番号>/comments
      gh api repos/<owner>/<repo>/issues/<PR番号>/comments
      ```
      ```
      git add <変更したファイル>
      git commit -m "<コミットメッセージ>"
      git push
      ```
   7. レビューで問題がなくなったら，Draftを解除する
      ```
      gh pr ready <PR番号> --repo <owner>/<repo>
      ```
   8. closeキーワード (`Closes owner/repo#番号`) は1箇所のPRのみに付与する．issueが存在するリポジトリのPR，またはユーザーが指定したPRに付与する (PR作成時の`--body`，またはマージ前に`gh pr edit <PR番号> --repo <owner>/<repo> --body "<本文>"`で追記する)
   9. それ以外のリポジトリのPRは `Related to owner/repo#番号` のみを記載し，closeキーワードは使わない
5. マージはユーザー自身が行う (共有状態を変更する操作のため，スキルは代行しない)
6. マージ完了の報告を受けたら，issueがクローズされたことを確認し，各リポジトリのローカルを最新化してローカルブランチを削除する
   ```
   gh issue view <番号> --repo <owner>/<repo>
   git checkout <デフォルトブランチ>
   git pull
   git branch -d fix/issue-<番号>-<内容を表す短い語句>
   ```

## 注意

- 1issueに対して複数のPRがある場合，closeキーワードを持つPRは1つだけにする
- 影響リポジトリが不明な場合は，ユーザーに確認する
- ブランチ作成・コミット・push・PR作成は実行前に都度確認する必要はないが，破壊的操作 (force push，reset --hard等) とマージは必ずユーザーに確認する
