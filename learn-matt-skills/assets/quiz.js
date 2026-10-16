// Reusable multiple-choice quiz with instant feedback.
// Usage: <div class="quiz" id="q"></div>
//        <script>Quiz.mount('q', [{ q: '...', options: ['a','b','c','d'], answer: 2, why: '...' }])</script>
(function () {
  const css = `
    .quiz .item { margin: 1.4rem 0; }
    .quiz .prompt { margin-bottom: .5rem; }
    .quiz .opts { display: grid; gap: .4rem; }
    .quiz .opts button { text-align: left; font-family: var(--mono); font-size: .85rem; }
    .quiz .opts button.right { border-color: var(--good); color: var(--good); }
    .quiz .opts button.wrong { border-color: var(--bad); color: var(--bad); text-decoration: line-through; }
    .quiz .why { font-size: .9rem; color: var(--muted); margin-top: .4rem; }
    .quiz .score { font-style: italic; margin-top: 1.5rem; }`;
  const style = document.createElement('style');
  style.textContent = css;
  document.head.appendChild(style);

  function mount(id, questions) {
    const root = document.getElementById(id);
    let done = 0, right = 0;
    const score = document.createElement('p');
    score.className = 'score';

    questions.forEach((item, i) => {
      const box = document.createElement('div');
      box.className = 'item';
      box.innerHTML = `<div class="prompt"><strong>${i + 1}.</strong> ${item.q}</div>`;
      const opts = document.createElement('div');
      opts.className = 'opts';
      const why = document.createElement('div');
      why.className = 'why';
      let answered = false;

      item.options.forEach((text, j) => {
        const b = document.createElement('button');
        b.textContent = text;
        b.onclick = () => {
          if (answered) return;
          if (j === item.answer) {
            answered = true;
            b.classList.add('right');
            why.innerHTML = item.why || '';
            done++;
            if (!b.dataset.missed) right++;
            score.textContent = done === questions.length
              ? `Done: ${right} of ${questions.length} right first time.`
              : '';
          } else {
            b.classList.add('wrong');
            opts.querySelectorAll('button')[item.answer].dataset.missed = '1';
          }
        };
        opts.appendChild(b);
      });

      box.appendChild(opts);
      box.appendChild(why);
      root.appendChild(box);
    });
    root.appendChild(score);
  }

  window.Quiz = { mount };
})();
