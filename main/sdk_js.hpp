#pragma once
// JS SDK, отдаётся по /sdk.js. Игры на SD подключают его: <script src="/sdk.js"></script>
static const char SDK_JS[] = R"JS(
(function(){
var P=new URLSearchParams(location.search);
var tok=P.get('t')||'';
try{if(tok)localStorage.setItem('cp_token',tok);else tok=localStorage.getItem('cp_token')||'';}catch(e){}
var CP=window.CP={token:tok,room:P.get('room')?+P.get('room'):0,members:[],owner:'',name:'',game:'',ws:null,auto:true,h:{}};
CP.setToken=function(t){CP.token=t;try{localStorage.setItem('cp_token',t)}catch(e){}};
CP.api=function(path,body){
  var o={method:body?'POST':'GET',headers:{'X-Token':CP.token||''}};
  if(body){o.headers['Content-Type']='application/json';o.body=JSON.stringify(body);}
  return fetch(path,o).then(function(r){return r.json().then(function(j){j._s=r.status;return j;});})
    .catch(function(){return {ok:false,error:'Нет связи с сервером'};});
};
CP.on=function(t,f){CP.h[t]=f;return CP;};
function emit(t,d){var f=CP.h[t];if(f)f(d);}
CP.connect=function(){
  if(CP.ws||!CP.token)return;
  var ws=new WebSocket('ws://'+location.host+'/ws');CP.ws=ws;
  ws.onopen=function(){ws.send(JSON.stringify({t:'hello',token:CP.token}));};
  ws.onmessage=function(e){
    var m;try{m=JSON.parse(e.data);}catch(x){return;}
    if(m.t==='welcome'){CP.name=m.name;CP.game=m.game||CP.game;CP.room=m.room||0;CP.members=m.members||[];CP.owner=m.owner||'';}
    else if(m.t==='join'||m.t==='leave'){CP.members=m.members||[];CP.owner=m.owner||'';}
    emit(m.t,m);
  };
  ws.onclose=function(){CP.ws=null;emit('close',{});if(CP.auto)setTimeout(CP.connect,1500);};
};
CP.send=function(d){if(CP.ws&&CP.ws.readyState===1)CP.ws.send(JSON.stringify({t:'msg',d:d}));};
CP.start=function(){if(CP.ws&&CP.ws.readyState===1)CP.ws.send(JSON.stringify({t:'start'}));};
CP.leave=function(){return CP.api('/api/room/leave',{});};
setInterval(function(){if(CP.ws&&CP.ws.readyState===1)CP.ws.send('{"t":"ping"}');},15000);
})();
)JS";
