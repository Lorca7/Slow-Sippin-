/* worker.js — renders one output channel in the background. */
importScripts('engine.js');

self.onmessage = function (e) {
  const msg = e.data;
  if (msg.type === 'ping') {
    self.postMessage({ type: 'pong' });
    return;
  }
  if (msg.type !== 'run') return;
  try {
    const job = new self.Stretch.StretchJob(msg.input, msg.opts);
    let last = 0;
    while (!job.done) {
      job.step(8);
      const now = Date.now();
      if (now - last > 80) {
        last = now;
        self.postMessage({ type: 'progress', p: job.progress });
      }
    }
    self.postMessage({ type: 'done', out: job.out }, [job.out.buffer]);
  } catch (err) {
    self.postMessage({ type: 'error', message: String((err && err.message) || err) });
  }
};
