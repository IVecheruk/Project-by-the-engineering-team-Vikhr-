(function () {
  // Настройки
  const minWheelDelta = 8; // можно оставить, чтобы тачпад не дрожал

  let index = 0;
  let slides = [];

  // 1) Находим исходный контейнер с контентом
  const sourceWrap = document.querySelector('.wrap');
  if (!sourceWrap) return console.warn('[slides] .wrap не найден, собирать нечего');

  // 2) Разбиваем детей на группы по <hr class="slide-sep">
  const groups = chunkBySeparators(Array.from(sourceWrap.childNodes), isSlideSeparator);
  if (groups.length === 0) groups.push(Array.from(sourceWrap.childNodes));

  // 3) Строим колоду
  const deck = document.createElement('div');
  deck.className = 'deck';
  document.body.appendChild(deck);

  groups.forEach((nodes, i) => {
    const slide = document.createElement('section');
    slide.className = 'slide' + (i === 0 ? ' is-active' : '');
    const inner = document.createElement('div');
    inner.className = 'wrap';
    nodes.forEach(n => inner.appendChild(n));
    slide.appendChild(inner);
    deck.appendChild(slide);
  });

  sourceWrap.remove();

  // 4) Точки навигации
  const dotsWrap = document.createElement('div');
  dotsWrap.className = 'dots';
  document.body.appendChild(dotsWrap);

  slides = Array.from(deck.querySelectorAll('.slide'));
  slides.forEach((_, i) => {
    const dot = document.createElement('div');
    dot.className = 'dot' + (i === index ? ' active' : '');
    dot.addEventListener('click', () => jumpTo(i));
    dotsWrap.appendChild(dot);
  });

  function updateDots() {
    dotsWrap.querySelectorAll('.dot').forEach((d, i) => d.classList.toggle('active', i === index));
  }

  // 5) МГНОВЕННОЕ переключение без анимаций
  function jumpTo(next) {
    if (next === index || next < 0 || next >= slides.length) return;
    slides[index].classList.remove('is-active');
    index = next;
    slides[index].classList.add('is-active');
    updateDots();
  }

  // Колесо мыши
  window.addEventListener('wheel', (e) => {
    if (Math.abs(e.deltaY) < minWheelDelta) return;
    if (e.deltaY > 0 && index < slides.length - 1) jumpTo(index + 1);
    else if (e.deltaY < 0 && index > 0) jumpTo(index - 1);
  }, { passive: true });

  // Клавиатура
  window.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown' || e.key === 'PageDown') jumpTo(Math.min(index + 1, slides.length - 1));
    else if (e.key === 'ArrowUp'   || e.key === 'PageUp')   jumpTo(Math.max(index - 1, 0));
    else if (e.key === 'Home') jumpTo(0);
    else if (e.key === 'End')  jumpTo(slides.length - 1);
  });

  // Тач-свайпы
  let touchStartY = null;
  window.addEventListener('touchstart', e => {
    if (e.touches && e.touches[0]) touchStartY = e.touches[0].clientY;
  }, { passive: true });
  window.addEventListener('touchend', e => {
    if (touchStartY === null) return;
    const endY = (e.changedTouches && e.changedTouches[0]) ? e.changedTouches[0].clientY : touchStartY;
    const dy = endY - touchStartY;
    const threshold = 40;
    if (dy < -threshold && index < slides.length - 1) jumpTo(index + 1);
    else if (dy > threshold && index > 0) jumpTo(index - 1);
    touchStartY = null;
  }, { passive: true });

  // Вспомогательные
  function isSlideSeparator(node) {
    return node.nodeType === 1 && node.matches && node.matches('hr.slide-sep');
  }

  function chunkBySeparators(nodes, isSep) {
    const chunks = [];
    let current = [];
    nodes.forEach(n => {
      if (isSep(n)) {
        if (current.length) chunks.push(current), current = [];
      } else {
        if (!(n.nodeType === 3 && !n.textContent.trim())) current.push(n);
      }
    });
    if (current.length) chunks.push(current);
    return chunks;
  }
})();
