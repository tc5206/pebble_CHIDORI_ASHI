# ちどりあし
自宅最寄り駅までの終電を検索するアプリ
## なにができる？
1. 飲み屋の最寄駅でアプリを起動し
2. 検索結果が出たら
3. upキー長押しでアラーム登録
4. downキー長押しでタイムラインにも登録
5. 終電の時間が近づいたらPebbleが教えてくれる！
## アプリを使う前の下準備
APIで使う駅IDを登録する

（例URL）[https://api.transit.ls8h.com/api/v1/locations/suggest?q=東京](https://api.transit.ls8h.com/api/v1/locations/suggest?q=東京)のように駅名を入れると

（例ID）`scrape-jreast-keihin-tohoku:odpt.Station:JR-East.KeihinTohokuNegishi.Tokyo`のように駅IDが出るので

それをApp SettingsのDestination Station ID欄に貼る
