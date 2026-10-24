#pragma once
// Встроенная страница captive portal. Если на SD есть /www/index.html — используется он.
static const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CardPutter Games</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:#12151c;color:#e8eaf0}
.w{max-width:480px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:8px 0 14px}
.card{background:#1c212c;border-radius:12px;padding:14px;margin-bottom:12px}
input{width:100%;padding:12px;margin:6px 0;border-radius:8px;border:1px solid #333b4d;background:#12151c;color:#fff;font-size:16px}
button{padding:12px 14px;border:0;border-radius:8px;background:#3d7bff;color:#fff;font-size:15px;margin:4px 6px 4px 0}
button.g{background:#2b3345}button.r{background:#c0392b}
.err{color:#ff7b7b;min-height:20px;font-size:14px}
.muted{color:#8b93a7;font-size:13px;margin:4px 0}
.row{display:flex;justify-content:space-between;align-items:center}
#hdr{display:none;margin-bottom:10px}
.tag{background:#2b3345;border-radius:6px;padding:2px 8px;font-size:12px;margin-left:6px}
</style></head><body><div class="w">
<h1>🎮 CardPutter Games</h1>
<div id="hdr" class="row"><div>👤 <b id="who"></b></div><div><button class="g" onclick="logout()">Выйти</button></div></div>
<div id="err" class="err"></div>

<div id="vAuth" style="display:none">
  <div class="card">
    <input id="n" placeholder="Имя (латиница, 3–16)" autocapitalize="off" autocomplete="username">
    <input id="p" type="password" placeholder="Пароль" autocomplete="current-password">
    <button onclick="doLogin()">Войти</button>
    <button class="g" onclick="doReg()">Регистрация</button>
  </div>
  <div class="card">
    <div class="muted">Без регистрации: прогресс не сохраняется.</div>
    <button class="g" onclick="doGuest()">Играть как гость</button>
  </div>
</div>

<div id="vGames" style="display:none">
  <div class="muted" id="cnt"></div>
  <div id="gl"></div>
</div>

<div id="vLobby" style="display:none">
  <div class="card">
    <div class="row"><b id="lt"></b><button class="g" onclick="backGames()">← Игры</button></div>
    <button onclick="quick()">⚡ Быстрый поиск</button>
    <input id="rt" placeholder="Название комнаты (необязательно)" maxlength="24">
    <button class="g" onclick="createRoom()">Создать комнату</button>
  </div>
  <div class="muted">Открытые комнаты:</div>
  <div id="rl"></div>
</div>

<div id="vRoom" style="display:none">
  <div class="card">
    <div class="row"><b id="rtitle"></b><span class="muted" id="rcap"></span></div>
    <div id="mem"></div>
    <div id="rbtn"></div>
    <div class="muted" id="rhint"></div>
  </div>
</div>
</div>
<script src="/sdk.js"></script>
<script>
var $=function(i){return document.getElementById(i)},cur='',curTitle='',timer=null,me=null,R=null,games=[];
function e(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function show(v){['vAuth','vGames','vLobby','vRoom'].forEach(function(x){$(x).style.display=x===v?'block':'none'});$('hdr').style.display=v==='vAuth'?'none':'flex';if(v!=='vLobby'&&timer){clearInterval(timer);timer=null}}
function err(t){$('err').textContent=t||''}
function auth(path,body){err('');CP.api(path,body).then(function(j){if(!j.ok){err(j.error||'Ошибка');return}CP.setToken(j.token);boot()})}
function doLogin(){auth('/api/login',{name:$('n').value.trim(),pass:$('p').value})}
function doReg(){auth('/api/register',{name:$('n').value.trim(),pass:$('p').value})}
function doGuest(){auth('/api/guest',{})}
function logout(){CP.auto=false;if(CP.ws)CP.ws.close();CP.api('/api/logout',{}).then(function(){CP.setToken('');location.reload()})}
function boot(){
  if(!CP.token){show('vAuth');return}
  CP.api('/api/me').then(function(j){
    if(!j.ok){CP.setToken('');show('vAuth');return}
    me=j;$('who').textContent=j.name+(j.guest?' (гость)':'');
    CP.auto=true;CP.connect();
    loadGames(function(){ if(j.room){cur=j.room.game;showRoom(j.room)}else show('vGames') });
  });
}
function loadGames(cb){
  CP.api('/api/games').then(function(g){
    games=g.games||[];var h='';
    games.forEach(function(x){
      h+='<div class="card"><b>'+e(x.title)+'</b><span class="tag">'+x.min+(x.max>x.min?'–'+x.max:'')+' игр.</span>'
        +'<div class="muted">'+e(x.description)+'</div>';
      if(x.min<=1)h+='<button onclick="solo(\''+e(x.id)+'\')">Играть одному</button>';
      if(x.max>1)h+='<button class="g" onclick="lobby(\''+e(x.id)+'\')">Найти игроков</button>';
      h+='</div>';
    });
    $('gl').innerHTML=h||'<div class="card">На SD-карте нет игр (папка /l18games/games).</div>';
    $('cnt').textContent=g.sd===false?'SD-карта не найдена':'';
    if(cb)cb();
  });
}
function gameTitle(id){for(var i=0;i<games.length;i++)if(games[i].id===id)return games[i].title;return id}
function solo(id){location.href='/g/'+id+'/?t='+CP.token}
function backGames(){show('vGames')}
function lobby(id){cur=id;$('lt').textContent=gameTitle(id);err('');show('vLobby');refreshRooms();timer=setInterval(refreshRooms,3000)}
function refreshRooms(){
  CP.api('/api/rooms?game='+cur).then(function(j){
    var h='';(j.rooms||[]).forEach(function(r){
      if(!r.open)return;
      h+='<div class="card row"><div><b>'+e(r.title)+'</b><div class="muted">Хост: '+e(r.owner)+' · '+r.count+'/'+r.max+'</div></div>'
        +(r.count<r.max?'<button onclick="join('+r.id+')">Войти</button>':'<span class="muted">полная</span>')+'</div>';
    });
    $('rl').innerHTML=h||'<div class="muted">Пока нет комнат — создайте свою или нажмите «Быстрый поиск».</div>';
  });
}
function roomAct(path,body){err('');CP.api(path,body).then(function(j){if(!j.ok){err(j.error||'Ошибка');return}showRoom(j.room)})}
function quick(){roomAct('/api/room/quick',{game:cur})}
function createRoom(){roomAct('/api/room/create',{game:cur,title:$('rt').value})}
function join(id){roomAct('/api/room/join',{id:id})}
function showRoom(r){R=r;cur=r.game;renderRoom();show('vRoom')}
function renderRoom(){
  if(!R)return;
  $('rtitle').textContent=R.title+' — '+gameTitle(R.game);
  $('rcap').textContent=R.members.length+'/'+R.max;
  var h='';R.members.forEach(function(m){h+='<div>'+(m===R.owner?'👑 ':'👤 ')+e(m)+'</div>'});
  $('mem').innerHTML=h;
  var host=me&&me.name===R.owner,b='';
  if(host)b+='<button onclick="startGame()"'+(R.members.length<2?' disabled':'')+'>▶ Старт</button>';
  b+='<button class="r" onclick="leaveRoom()">Покинуть</button>';
  $('rbtn').innerHTML=b;
  $('rhint').textContent=host?(R.members.length<2?'Ждём второго игрока…':'Можно стартовать'):'Ждём, пока хост начнёт игру…';
}
function startGame(){CP.start()}
function leaveRoom(){CP.leave().then(function(){R=null;lobby(cur)})}
CP.on('join',function(m){if(R){R.members=m.members;R.owner=m.owner;renderRoom()}});
CP.on('leave',function(m){if(R){R.members=m.members;R.owner=m.owner;renderRoom()}});
CP.on('start',function(m){location.href='/g/'+m.game+'/?room='+m.room+'&t='+CP.token});
CP.on('error',function(m){if(m.e==='auth'){CP.setToken('');show('vAuth')}else err(m.e)});
boot();
</script></body></html>
)HTML";
