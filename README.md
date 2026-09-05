# ちどりあし
自宅最寄り駅までの終電を検索するアプリ
## 最初に知っておいてね
- TRANSIT APIで駅IDを取得しておく必要がある
- アプリを起動した地点から約200m以内の駅を検索するので飲み屋に入る前、駅に着いた時点でセットすること
- 検索結果は必ずしも正しいとは限らないので終電ギリギリを攻めすぎないように
- アラーム鳴動時のアニメーションは公式が作ったタイマー鳴動時のアニメーションを流用した
- 路線バスは検索対象外とした（鉄道よりも終バスが早い場合が多いため）
## なにができる？
1. **飲み屋の最寄駅に着いた時点で**アプリを起動し
2. 検索結果が出たら
3. upキー長押しで発車30分前（デフォルト）のアラーム登録
4. downキー長押しでタイムラインにも発車時刻を登録
5. 登録が済んだらアプリは閉じてOK
6. 終電の時間が近づいたらPebbleが教えてくれる！
### アプリを使う前の下準備（App Settings）
APIで使う駅ID（自宅最寄駅の駅ID）を登録する<br>（例URL）[https://api.transit.ls8h.com/api/v1/locations/suggest?q=東京](https://api.transit.ls8h.com/api/v1/locations/suggest?q=東京)のように駅名を入れると<br>（例ID）`scrape-jreast-keihin-tohoku:odpt.Station:JR-East.KeihinTohokuNegishi.Tokyo`のように駅IDが出るので<br>それをDestination Station ID欄に貼る
#### 他にも設定できること
- 終電が0:15着だけど一本早い電車で着きたい場合はArrival Search Time欄を24:14にすると0:14着以前の電車を探してくれる<br>もっと早い時間を設定することも可能
- デフォルトでは乗車30分前にアラームが鳴動するがAlarm Minutes Before欄を他の値に変更できる（単位：分）
## 操作方法
- `UP`検索結果ひとつ戻る
- `DOWN`検索結果ひとつ進む
- `SELECT`再検索
- `Long UP`表示された検索結果の発車30分前アラームセット/アラーム解除
- `Long SELECT`セットされたアラーム解除
- `Long DOWN`表示された検索結果の発車時刻にタイムラインへのピン打ち
## 注意点
アラームをセットするとタイムラインに「終電30分前」と表示されるが、これを削除してもアラームは解除されない<br>アプリで`Long UP`か`Long SELECT`をしてアラームを解除する必要がある
# ダウンロード
[Chidori-ashi.pbw v1.11](https://github.com/tc5206/pebble_CHIDORI_ASHI/blob/main/Chidori-ashi.pbw)
<br>サイドロードしてつかってね
