---
name: issue-resolve
description: GitHub issueに対応する。1issue=1回の実行で，複数リポジトリにまたがる修正もスキル内部で完結させる。「issue #Nに対応して」で起動。
allowed-tools: Edit, Write, Bash(git add *) Bash(git commit *) Bash(git push *) Bash(git checkout *) Bash(git pull) Bash(git branch -d *) Bash(gh pr create *) Bash(gh pr edit *) Bash(gh api *) Bash(gh issue view *) Bash(claude -p --tools "Read,Grep,Glob" -- "*" > review.md)
---

# 概要

GitHub issueへの対応 (調査・ブランチ作成・修正・PR作成) を行う．粒度は1issue=1回の実行．対応が複数リポジトリにまたがる場合も，このスキル内で完結させる．

# 手順

## 1. issue内容の確認

```
gh issue view <番号> --repo <owner>/<repo>
```

## 2. 調査

原因調査を行い，影響範囲 (対象リポジトリ・対象ファイル) を特定する．issue本文中に推測が書かれている場合は，事実かどうかをここで検証する．

## 3. 修正方針の承認

調査結果と修正方針をユーザーに提示し，承認を得る．**これが実装前の最後の確認ポイント**であり，承認後は手順4を人間の追加確認なしに連続して実行する．  
ここでは，特に聞かれない限り具体的なソース内容を提示する必要はない．ソースのうち，どの仕事をする部分をどのように修正するかを提示できれば十分．  

## 4. リポジトリごとの実装〜PR作成

影響リポジトリごとに以下を行う．

### 4.1 ブランチ作成

```
git checkout <デフォルトブランチ>
git pull
git checkout -b fix/issue-<番号>-<内容を表す短い語句>
```

### 4.2 修正

手順3で承認された方針に基づいて修正を行う．

### 4.3 コミット・push・PR作成

```
git add <変更したファイル>
git commit -m "<コミットメッセージ>"
git push -u origin fix/issue-<番号>-<内容を表す短い語句>
gh pr create --repo <owner>/<repo> --title "<タイトル>" --body "<本文>" [--draft]
```

PR作成時の`--body`に，closeキーワード (`Closes owner/repo#番号`) またはリンクのみ (`Related to owner/repo#番号`) を含める．closeキーワードは1issueにつき1箇所のPRのみに付与する (issueが存在するリポジトリのPR，またはユーザーが指定したPR)．それ以外のリポジトリのPRは`Related to owner/repo#番号`のみを記載する．  

**`--draft`は，closeキーワードを持つPRにのみ付ける．** マージするとissueが閉じるPRであることを一目で分かるようにするための運用．closeキーワードを持たない(`Related to`のみの)PRはissueを閉じないため，通常のPR(Ready)として作成する．  

### 4.4 自動レビューループ

`claude -p`によるレビューを行い，重大な指摘が無くなるまで，人間の確認なしに修正・再レビューを繰り返す．変更内容のうち説明が必要な箇所は，PRへのコメントまたはコミットメッセージで補足する．  
一回のレビュー内容のうち，指摘事項一つずつについてコミットを行う．プッシュについてはまとめて行ってよい．  
レビューで指摘された項目について，対応する必要がないと判断したものについては対応しなくてよい．ただし，指摘されたが無視されたことはコミットメッセージに必ず含める．  
レビュー結果の修正が他のプロジェクトにも及ぶ場合は，同じissueに対応する設定のブランチを切ってから対応する．  

```
claude -p --tools "Read,Grep,Glob" -- "コードレビューを依頼する文言" > review.md
```
```
git add <変更したファイル>
git commit -m "<コミットメッセージ>"
git push
```

### 4.5 人間レビューへの対応

Draftの解除はユーザーが手動で行う (AIは行わない)．  

ユーザーがPRにレビューコメントを追加した場合は取得して確認し，指摘があれば修正してコミット・pushする．

```
gh api repos/<owner>/<repo>/pulls/<PR番号>/comments
gh api repos/<owner>/<repo>/issues/<PR番号>/comments
```
```
git add <変更したファイル>
git commit -m "<コミットメッセージ>"
git push
```

## 5. マージ

マージはユーザー自身が行う (共有状態を変更する操作のため，スキルは代行しない)．  

## 6. マージ後の後始末

マージ完了の報告を受けたら，issueがクローズされたことを確認し，各リポジトリのローカルを最新化してローカルブランチを削除する．  

```
gh issue view <番号> --repo <owner>/<repo>
git checkout <デフォルトブランチ>
git pull
git branch -d fix/issue-<番号>-<内容を表す短い語句>
```

# 注意

- 人間の確認が必須なのは，手順3(修正方針の承認)と，破壊的操作(force push，reset --hard等)のみ．それ以外(ブランチ作成・実装・コミット・push・PR作成・レビューループ・レビューコメント対応)は連続して自動実行する
- 1issueに対して複数のPRがある場合，closeキーワードを持つPRは1つだけにする
- 影響リポジトリが不明な場合は，ユーザーに確認する
- マージは決して行わない
