/* 团购推荐助手 — 前端逻辑（Vanilla JS，无依赖）
 *
 * 与后端契约：
 *  - POST /v1/chat/stream  (SSE: data: {"event","data"}\n\n, token 流式)
 *  - POST /v1/chat         (一次性 JSON，流式失败时降级)
 *  - GET  /v1/health       (状态点)
 * 原生 EventSource 只支持 GET，故流式用 fetch + ReadableStream 手动解析。
 */
(function () {
  'use strict';

  // ---- 状态 ----
  var state = {
    messagesEl: document.getElementById('messages'),
    inputEl: document.getElementById('input'),
    sendBtn: document.getElementById('send'),
    stopBtn: document.getElementById('stop'),
    statusEl: document.getElementById('status'),
    statusDot: document.getElementById('status-dot'),
    statusText: document.getElementById('status-text'),
    sessionId: null,
    streaming: false,
    controller: null,
    userId: (function () {
      // Phase 2.2: 持久匿名用户 ID,支撑跨会话用户画像与 A/B 分桶。
      try {
        var id = window.localStorage.getItem('agent_user_id');
        if (!id) {
          id = (window.crypto && crypto.randomUUID)
            ? crypto.randomUUID()
            : 'u-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 10);
          window.localStorage.setItem('agent_user_id', id);
        }
        return id;
      } catch (e) { return 'web-user'; }
    })()
  };

  try {
    state.sessionId = window.localStorage.getItem('agent_session_id') || null;
  } catch (e) { /* localStorage 不可用时忽略 */ }

  var TOOL_LABELS = {
    deal_retriever: '商品召回',
    deal_ranker: '智能排序',
    kb_search: '知识检索'
  };

  // ---- 小工具 ----
  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
  }

  function scrollToBottom() {
    state.messagesEl.scrollTop = state.messagesEl.scrollHeight;
  }

  function persistSession(id) {
    state.sessionId = id;
    try { window.localStorage.setItem('agent_session_id', id); } catch (e) {}
  }

  // ---- 空态欢迎 ----
  var EXAMPLES = [
    '武汉3人想吃小龙虾，预算400，能开发票吗',
    '上海3人吃海鲜，预算300左右',
    '周末带家人吃火锅，4个人，北京，500预算',
    '团购券可以开发票吗？'
  ];

  function renderWelcome() {
    var w = el('div', 'welcome');
    w.appendChild(el('div', 'welcome-icon', '🛒'));
    w.appendChild(el('h2', null, '你好，我是团购推荐助手'));
    w.appendChild(el('p', null, '告诉我城市、人数、预算和口味，我帮你挑高性价比的团购，还能解答发票、包间、停车等问题。'));
    var chips = el('div', 'welcome-chips');
    EXAMPLES.forEach(function (t) {
      var c = el('span', 'chip', t);
      c.addEventListener('click', function () { sendMessage(t); });
      chips.appendChild(c);
    });
    w.appendChild(chips);
    state.messagesEl.appendChild(w);
  }

  function clearWelcome() {
    var w = state.messagesEl.querySelector('.welcome');
    if (w) w.remove();
  }

  // ---- 消息渲染 ----
  function addMessage(role, contentNode) {
    var msg = el('div', 'msg ' + role);
    if (role !== 'system') {
      var av = el('div', 'avatar', role === 'user' ? '🧑' : '🤖');
      msg.appendChild(av);
    }
    msg.appendChild(contentNode);
    state.messagesEl.appendChild(msg);
    scrollToBottom();
    return msg;
  }

  function renderUserBubble(text) {
    var b = el('div', 'bubble', text);
    addMessage('user', b);
  }

  function renderSystemNote(text, isError) {
    var b = el('div', 'bubble', text);
    addMessage(isError ? 'error' : 'system', b);
  }

  // 创建助手消息骨架，返回可操作句柄
  function newAssistantMessage() {
    var bubble = el('div', 'bubble');
    var pipeline = el('details', 'pipeline');
    var summary = el('summary');
    var title = el('span', 'pipeline-title');
    title.appendChild(el('span', 'spinner'));
    title.appendChild(el('span', 'status-label', '正在思考…'));
    summary.appendChild(el('span', 'chev', '▶'));
    summary.appendChild(title);
    var steps = el('ul', 'pipeline-steps');
    pipeline.appendChild(summary);
    pipeline.appendChild(steps);
    pipeline.style.display = 'none';   // 有工具调用时才显示

    var content = el('div', 'content');

    bubble.appendChild(pipeline);
    bubble.appendChild(content);
    addMessage('assistant', bubble);

    return {
      bubble: bubble,
      pipeline: pipeline,
      steps: steps,
      statusLabel: title.querySelector('.status-label'),
      spinner: title.querySelector('.spinner'),
      content: content,
      text: ''
    };
  }

  function pipelineStep(handle, toolName, done) {
    handle.pipeline.style.display = '';
    var li = el('li');
    var tick = el('span', 'tick' + (done ? '' : ' busy'), done ? '✓' : '…');
    var label = el('span', null, TOOL_LABELS[toolName] || toolName);
    li.appendChild(tick);
    li.appendChild(label);
    li.dataset.tool = toolName;
    handle.steps.appendChild(li);
    scrollToBottom();
  }

  function pipelineComplete(handle, label) {
    var sp = handle.spinner;
    if (sp) sp.remove();
    handle.statusLabel.textContent = label || '思考过程';
  }

  function appendDelta(handle, delta) {
    if (handle.text === '') {
      handle.content.classList.add('typing-caret');
    }
    handle.text += delta;
    handle.content.textContent = handle.text;
    scrollToBottom();
  }

  function finalizeText(handle, finalReply) {
    if (finalReply && finalReply !== handle.text) {
      handle.text = finalReply;
      handle.content.textContent = finalReply;
    }
    handle.content.classList.remove('typing-caret');
  }

  // ---- 商品卡片 ----
  function formatDiscount(item) {
    if (!item.original_price || item.original_price <= item.price || item.price <= 0) return null;
    var zhe = (item.price / item.original_price) * 10;
    return zhe.toFixed(1) + ' 折';
  }

  function renderDealCards(handle, items) {
    if (!items || !items.length) return;
    var wrap = el('div', 'cards');
    items.forEach(function (it) {
      var card = el('div', 'card');

      var head = el('div', 'card-head');
      head.appendChild(el('div', 'card-title', it.title || '团购套餐'));
      if (it.category) head.appendChild(el('div', 'card-cat', it.category));
      card.appendChild(head);

      var priceRow = el('div', 'card-price');
      var price = el('span', 'price');
      price.appendChild(el('span', 'currency', '¥'));
      price.appendChild(document.createTextNode(String(it.price)));
      priceRow.appendChild(price);
      if (it.original_price && it.original_price > it.price) {
        priceRow.appendChild(el('span', 'price-original', '¥' + it.original_price));
      }
      var disc = formatDiscount(it);
      if (disc) priceRow.appendChild(el('span', 'discount', disc));
      card.appendChild(priceRow);

      var meta = el('div', 'card-meta');
      if (it.rating) meta.appendChild(el('span', 'rating', '⭐ ' + Number(it.rating).toFixed(1)));
      if (it.sold_count) meta.appendChild(el('span', null, '🔥 已售 ' + it.sold_count));
      if (it.district) meta.appendChild(el('span', null, '📍 ' + it.district));
      else if (it.city) meta.appendChild(el('span', null, '📍 ' + it.city));
      if (meta.childNodes.length) card.appendChild(meta);

      if (it.tags && it.tags.length) {
        var tags = el('div', 'card-tags');
        it.tags.slice(0, 4).forEach(function (t) { tags.appendChild(el('span', 'card-tag', t)); });
        card.appendChild(tags);
      }

      if (it.reason) card.appendChild(el('div', 'card-reason', it.reason));

      card.appendChild(feedbackButtons(it.item_id || ''));

      wrap.appendChild(card);
    });
    handle.bubble.appendChild(wrap);
    scrollToBottom();
  }

  // ---- 反馈（👍/👎 → /v1/feedback 落库） ----
  function sendFeedback(itemId, type, wrap, doneLabel) {
    if (!state.sessionId) return;
    var body = JSON.stringify({
      user_id: state.userId,
      session_id: state.sessionId,
      trace_id: state.lastTraceId || '',
      item_id: itemId,
      feedback_type: type
    });
    // 乐观置灰；失败则恢复可点
    Array.prototype.forEach.call(wrap.querySelectorAll('button'), function (b) {
      b.disabled = true;
    });
    fetch('/v1/feedback', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: body
    }).then(function (resp) {
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      wrap.textContent = doneLabel;
      wrap.classList.add('feedback-done');
    }).catch(function () {
      Array.prototype.forEach.call(wrap.querySelectorAll('button'), function (b) {
        b.disabled = false;
      });
    });
  }

  function feedbackButtons(itemId) {
    var wrap = el('div', itemId ? 'card-feedback' : 'feedback-bar');
    var up = el('button', 'feedback-btn', '👍');
    var down = el('button', 'feedback-btn', '👎');
    up.title = '有帮助';
    down.title = '不太好';
    up.addEventListener('click', function () { sendFeedback(itemId, 'like', wrap, '已收到反馈，谢谢！'); });
    down.addEventListener('click', function () { sendFeedback(itemId, 'dislike', wrap, '已收到反馈，我们会改进'); });
    wrap.appendChild(up);
    wrap.appendChild(down);
    return wrap;
  }

  function renderFeedbackBar(handle) {
    handle.bubble.appendChild(feedbackButtons(''));
    scrollToBottom();
  }

  // ---- 回答依据（RAG grounding） ----
  function renderGrounding(handle, grounding) {
    if (!grounding || !grounding.length) return;
    var det = el('details', 'grounding');
    det.appendChild(el('summary', null, '📚 回答依据（' + grounding.length + '）'));
    var ul = el('ul');
    grounding.forEach(function (g) { ul.appendChild(el('li', null, g)); });
    det.appendChild(ul);
    handle.bubble.appendChild(det);
    scrollToBottom();
  }

  // ---- 追问芯片 ----
  var SLOT_SUGGESTIONS = {
    city: ['上海', '北京', '武汉', '成都', '广州', '杭州'],
    people: ['2人', '3人', '4人', '6人'],
    budget: ['100元', '200元', '300元', '500元'],
    category: ['海鲜', '火锅', '小龙虾', '烧烤', '日料'],
    district: ['市中心', '学校附近']
  };

  function renderClarifyChips(handle, missingSlots) {
    var values = [];
    (missingSlots || []).forEach(function (slot) {
      var sug = SLOT_SUGGESTIONS[slot];
      if (sug) values = values.concat(sug.slice(0, 4));
    });
    if (!values.length) return;
    var wrap = el('div', 'clarify-chips');
    values.slice(0, 8).forEach(function (v) {
      var c = el('span', 'chip', v);
      c.addEventListener('click', function () {
        var cur = state.inputEl.value.trim();
        state.inputEl.value = cur ? cur + ' ' + v : v;
        state.inputEl.focus();
        autoResize();
      });
      wrap.appendChild(c);
    });
    handle.bubble.appendChild(wrap);
    scrollToBottom();
  }

  // ---- SSE 事件分派 ----
  function dispatchEvent(handle, ctx, evt, data) {
    switch (evt) {
      case 'started':
        if (data.session_id) persistSession(data.session_id);
        break;
      case 'planning':
        break;
      case 'plan':
        ctx.missingSlots = data.missing_slots || [];
        break;
      case 'tool_call':
        pipelineStep(handle, data.tool_name, false);
        break;
      case 'tool_result': {
        // 把对应步骤标记完成
        var items = handle.steps.querySelectorAll('li');
        for (var i = items.length - 1; i >= 0; i--) {
          if (items[i].dataset.tool === data.tool_name && items[i].querySelector('.tick.busy')) {
            var tick = items[i].querySelector('.tick');
            tick.textContent = data.success ? '✓' : '✗';
            tick.className = 'tick';
            break;
          }
        }
        break;
      }
      case 'grounding':
        ctx.hasGrounding = true;
        break;
      case 'composing':
        handle.statusLabel.textContent = '正在生成回复…';
        break;
      case 'delta':
        appendDelta(handle, data.content || '');
        break;
      case 'replace':
        // Phase 4-B:事实校验在流式输出完成后发现违规,服务端用本事件整体
        // 替换该条回复为模板兜底。旧前端无此 case 会走 default 静默忽略。
        handle.text = data.content || '';
        handle.content.textContent = handle.text;
        scrollToBottom();
        break;
      case 'final':
        ctx.final = data;
        break;
      case 'error':
        ctx.error = data.message || '出错了';
        break;
      default:
        break;
    }
  }

  // ---- 流式请求 ----
  async function streamChat(text, handle, ctx) {
    var body = JSON.stringify({
      user_id: state.userId,
      session_id: state.sessionId || '',
      message: text
    });

    var resp = await fetch('/v1/chat/stream', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: body,
      signal: state.controller.signal
    });
    if (!resp.ok || !resp.body) throw new Error('HTTP ' + resp.status);

    var reader = resp.body.getReader();
    var decoder = new TextDecoder('utf-8');
    var buffer = '';

    for (;;) {
      var chunk = await reader.read();
      if (chunk.done) break;
      buffer += decoder.decode(chunk.value, { stream: true });

      var idx;
      while ((idx = buffer.indexOf('\n\n')) !== -1) {
        var block = buffer.slice(0, idx);
        buffer = buffer.slice(idx + 2);
        var line = block.trim();
        if (line.indexOf('data:') !== 0) continue;
        var payload = line.slice(5).trim();
        if (!payload) continue;
        try {
          var env = JSON.parse(payload);
          dispatchEvent(handle, ctx, env.event, env.data || {});
        } catch (e) { /* 忽略畸形块 */ }
      }
    }
  }

  // ---- 非流式降级 ----
  async function fallbackChat(text, handle, ctx) {
    var body = JSON.stringify({
      user_id: state.userId,
      session_id: state.sessionId || '',
      message: text
    });
    var resp = await fetch('/v1/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: body
    });
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    var data = await resp.json();
    if (data.session_id) persistSession(data.session_id);
    handle.text = data.reply || '';
    handle.content.textContent = handle.text;
    ctx.final = data;
  }

  // ---- 完成渲染 ----
  function finish(handle, ctx) {
    pipelineComplete(handle, '思考过程');
    if (ctx.error) {
      finalizeText(handle, '');
      if (!handle.text) renderSystemNote('⚠️ ' + ctx.error, true);
    } else if (ctx.final) {
      var f = ctx.final;
      state.lastTraceId = f.trace_id || '';
      finalizeText(handle, f.reply || '');
      if (!handle.text && !f.is_clarifying) {
        handle.content.textContent = '（没有收到回复）';
      }
      renderDealCards(handle, f.items);
      renderGrounding(handle, f.grounding);
      if (f.is_clarifying) renderClarifyChips(handle, ctx.missingSlots);
      renderFeedbackBar(handle);
    } else if (!handle.text) {
      renderSystemNote('⚠️ 未收到有效响应', true);
    }
    setStreaming(false);
  }

  // ---- 发送 ----
  function setStreaming(on) {
    state.streaming = on;
    state.sendBtn.hidden = on;
    state.stopBtn.hidden = !on;
    state.inputEl.disabled = false;
  }

  async function sendMessage(text) {
    text = (text || '').trim();
    if (!text || state.streaming) return;

    clearWelcome();
    renderUserBubble(text);
    state.inputEl.value = '';
    autoResize();

    var handle = newAssistantMessage();
    var ctx = { missingSlots: [], final: null, error: null, hasGrounding: false };

    setStreaming(true);
    state.controller = new AbortController();

    try {
      await streamChat(text, handle, ctx);
    } catch (e) {
      if (e && e.name === 'AbortError') {
        finalizeText(handle, '');
        ctx.error = null;
        pipelineComplete(handle, '已停止');
        setStreaming(false);
        return;
      }
      // 流式失败 → 一次性降级
      try {
        await fallbackChat(text, handle, ctx);
      } catch (e2) {
        ctx.error = '无法连接服务，请稍后重试';
      }
    }
    finish(handle, ctx);
    state.inputEl.focus();
  }

  // ---- 输入框 ----
  function autoResize() {
    var t = state.inputEl;
    t.style.height = 'auto';
    t.style.height = Math.min(t.scrollHeight, 160) + 'px';
  }

  state.sendBtn.addEventListener('click', function () { sendMessage(state.inputEl.value); });
  state.stopBtn.addEventListener('click', function () {
    if (state.controller) state.controller.abort();
  });
  state.inputEl.addEventListener('input', autoResize);
  state.inputEl.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage(state.inputEl.value);
    }
  });

  // ---- 健康检查 ----
  function setOnline(online) {
    state.statusEl.classList.toggle('is-online', online);
    state.statusEl.classList.toggle('is-offline', !online);
    state.statusText.textContent = online ? '在线' : '离线';
  }

  async function checkHealth() {
    try {
      var r = await fetch('/v1/health', { cache: 'no-store' });
      setOnline(r.ok);
    } catch (e) {
      setOnline(false);
    }
  }

  // ---- 启动 ----
  renderWelcome();
  autoResize();
  checkHealth();
  setInterval(checkHealth, 10000);
  state.inputEl.focus();
})();
