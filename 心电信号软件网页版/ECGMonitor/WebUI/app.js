/**
 * ECG Monitor App — UI Controller v5.1
 * Full batch analysis, training, record navigation
 */
(function() {
    'use strict';

    // --- State ---
    var currentResult = null;
    var currentRecords = [];          // parsed CSV data
    var allResults = [];             // ECGEngine results for all records
    var currentRecordIndex = 0;
    var ecgChart = null;
    var isTraining = false;
    var _undoStack = [];
    var trainedParams = null;
    var STORAGE_KEY = 'ecg_monitor_results';
    var _db = null;

    // === IndexedDB (无限存储) ===
    function openDB() {
        if (_db) return Promise.resolve(_db);
        return new Promise(function(resolve) {
            var req = indexedDB.open('ECGMonitor', 1);
            req.onupgradeneeded = function(e) {
                var d = e.target.result;
                if (!d.objectStoreNames.contains('results')) d.createObjectStore('results', { keyPath: 'file' });
            };
            req.onsuccess = function(e) { _db = e.target.result; resolve(_db); };
            req.onerror = function() { resolve(null); };
        });
    }
    function saveResultsDB() {
        if (allResults.length === 0) return;
        openDB().then(function(db) { if(!db)return;
            var tx = db.transaction('results','readwrite'), st = tx.objectStore('results');
            allResults.forEach(function(r) {
                var inf = r.inference || { type:0, scores:[0,0,0,0,0,0] };
                st.put({ file: r.filename, time: new Date().toISOString().slice(0,19),
                    samples: r.rawData.length, peaks: r.numPeaks,
                    hr: r.stats.hrMean.toFixed(1), hrStd: r.stats.hrStd.toFixed(1),
                    rrMean: r.stats.rrMean.toFixed(1), cv: (r.stats.rrCV*100).toFixed(1),
                    label: r.truthLabel||'', type: ECGEngine.anomalyName(inf.type)
                });
            });
        });
    }
    function loadResultsDB(cb) {
        openDB().then(function(db) { if(!db) return cb([]);
            var req = db.transaction('results','readonly').objectStore('results').getAll();
            req.onsuccess = function(e) { cb(e.target.result || []); };
            req.onerror = function() { cb([]); };
        });
    }

    // --- Init ---
    document.addEventListener('DOMContentLoaded', function() {
        initDate();
        initNavigation();
        initUpload();
        initChart();
        initSettings();
        initExport();
        initRecordNav();
        initTraining();
        initDownloadBtn();
        initUndoPeak();
        loadDemoData();
    });

    function initDate() {
        var now = new Date();
        var days = ['日','一','二','三','四','五','六'];
        document.getElementById('dateDisplay').textContent =
            now.getFullYear() + '年' + (now.getMonth()+1) + '月' + now.getDate() + '日'
            + ' 星期' + days[now.getDay()];
    }

    // --- Navigation ---
    function initNavigation() {
        document.querySelectorAll('.nav-item').forEach(function(item) {
            item.addEventListener('click', function(e) {
                e.preventDefault();
                navigateTo(item.getAttribute('data-page'));
            });
        });
    }

    // --- Record Navigation ---
    function initRecordNav() {
        document.getElementById('btnPrev').addEventListener('click', function() {
            if (allResults.length < 2) return;
            currentRecordIndex = (currentRecordIndex - 1 + allResults.length) % allResults.length;
            showRecord(currentRecordIndex);
        });
        document.getElementById('btnNext').addEventListener('click', function() {
            if (allResults.length < 2) return;
            currentRecordIndex = (currentRecordIndex + 1) % allResults.length;
            showRecord(currentRecordIndex);
        });
    }

    function updateRecordNav() {
        var nav = document.getElementById('recordNav');
        var lbl = document.getElementById('lblRecordIdx');
        if (allResults.length > 1) {
            nav.style.display = 'flex';
            lbl.textContent = (currentRecordIndex + 1) + '/' + allResults.length;
        } else {
            nav.style.display = 'none';
        }
    }

    function showRecord(idx) {
        if (idx < 0 || idx >= allResults.length) return;
        currentRecordIndex = idx;
        currentResult = allResults[idx];
        updateDashboard();
        updateChart();
        updateTable();
        updateDiagnosis();
        updateReport();
        updateRecordNav();
    }

    // --- Upload ---
    function initUpload() {
        var zone = document.getElementById('uploadZone');
        var fileInput = document.getElementById('fileInput');

        zone.addEventListener('click', function() { fileInput.click(); });
        fileInput.addEventListener('change', handleFiles);

        zone.addEventListener('dragover', function(e) { e.preventDefault(); zone.classList.add('drag-over'); });
        zone.addEventListener('dragleave', function() { zone.classList.remove('drag-over'); });
        zone.addEventListener('drop', function(e) {
            e.preventDefault();
            zone.classList.remove('drag-over');
            handleFileList(e.dataTransfer.files);
        });

        document.getElementById('btnLoadFile').addEventListener('click', function() {
            fileInput.removeAttribute('multiple');
            fileInput.click();
        });
        document.getElementById('btnLoadDir').addEventListener('click', function() {
            fileInput.setAttribute('multiple', 'multiple');
            fileInput.click();
        });
    }

    function handleFiles(e) {
        handleFileList(e.target.files);
    }

    function handleFileList(files) {
        if (!files || files.length === 0) return;
        currentRecords = [];
        allResults = [];
        currentRecordIndex = 0;

        var fileListDiv = document.getElementById('fileList');
        var progressDiv = document.getElementById('loadProgress');
        var progressFill = document.getElementById('progressFill');

        fileListDiv.style.display = 'block';
        fileListDiv.innerHTML = '<p style="color:var(--text-secondary);font-size:13px">正在加载 ' + files.length + ' 个文件...</p>';
        progressDiv.style.display = 'block';

        var loaded = 0;
        var fileArray = Array.from(files);

        fileArray.forEach(function(file) {
            var fname = file.name;  // 快照，防止闭包陷阱
            // 跳过GT文件和manifest
            var fn = fname.toLowerCase();
            if (fn.indexOf('_gt.') >= 0 || fn.indexOf('manifest') >= 0) {
                loaded++;
                if (loaded === fileArray.length) finishLoading();
                return;
            }
            var reader = new FileReader();
            reader.onload = function(e) {
                loaded++;
                var data = ECGEngine.parseCSV(e.target.result);
                data.filename = fname.replace('.csv', '').replace(/_raw$/, '');
                currentRecords.push(data);
                progressFill.style.width = (loaded / fileArray.length * 100) + '%';
                if (loaded === fileArray.length) finishLoading();
            };
            reader.onerror = function() {
                loaded++;
                if (loaded === fileArray.length) finishLoading();
            };
            reader.readAsText(file);
        });
    }

    function finishLoading() {
        var progressDiv = document.getElementById('loadProgress');
        progressDiv.style.display = 'none';

        var fileListDiv = document.getElementById('fileList');
        var normalCount = 0;
        currentRecords.forEach(function(r) { if (r.label === 'normal') normalCount++; });

        var html = '<div style="margin-bottom:8px;color:var(--text-secondary);font-size:13px">'
            + '已加载 <b>' + currentRecords.length + '</b> 个文件'
            + ' (正常: ' + normalCount + ', 异常: ' + (currentRecords.length - normalCount) + ')'
            + '</div>';
        // 仅显示前200条，避免8000条DOM渲染卡顿
        var showCount = Math.min(currentRecords.length, 200);
        for (var idx = 0; idx < showCount; idx++) {
            var rec = currentRecords[idx];
            html += '<div class="file-item" data-idx="' + idx + '" style="cursor:pointer">'
                + '<span>' + rec.filename + ' ('
                + rec.values.length + '样本, '
                + rec.sampleRate + 'Hz)</span>'
                + '<span class="file-item-badge">' + (rec.label || '未标注') + '</span>'
                + '</div>';
        }
        if (currentRecords.length > 200) {
            html += '<div class="file-item" style="color:var(--text-secondary)">... 还有 ' + (currentRecords.length - 200) + ' 个文件未显示（分析全部时会处理）</div>';
        }
        html += '<div style="margin-top:10px;display:flex;gap:8px;flex-wrap:wrap">'
            + '<button class="btn btn-primary" id="btnAnalyzeAll">分析全部</button>'
            + '<small style="color:var(--text-secondary);align-self:center">训练数据: data/train/ (8000条)</small>'
            + '<span style="margin-left:4px;font-size:12px;color:var(--text-secondary);align-self:center" id="btnAnalyzeStatus"></span>'
            + '</div>';
        fileListDiv.innerHTML = html;

        // Click file item to analyze single
        fileListDiv.querySelectorAll('.file-item').forEach(function(item) {
            item.addEventListener('click', function() {
                var idx = parseInt(this.getAttribute('data-idx'));
                processSingle(idx);
                navigateTo('overview');
            });
        });

        document.getElementById('btnAnalyzeAll').addEventListener('click', function() {
            runBatchAnalysis();
        });
    }

    function processSingle(idx) {
        if (idx < 0 || idx >= currentRecords.length) return;
        var rec = currentRecords[idx];
        var sr = parseInt(document.getElementById('setSampleRate').value, 10) || rec.sampleRate;
        var result = ECGEngine.processECG(rec.values, sr);
        result.filename = rec.filename;
        result.truthLabel = rec.label;

        allResults[idx] = result;
        currentRecordIndex = idx;
        currentResult = result;
    }

    function processAndShow(record) {
        processSingle(currentRecords.indexOf(record));
        updateDashboard();
        updateChart();
        updateTable();
        updateDiagnosis();
        updateReport();
        updateRecordNav();
    }

    function runBatchAnalysis() {
        if (currentRecords.length === 0) return;
        var statusEl = document.getElementById('btnAnalyzeStatus');
        statusEl.textContent = '正在分析...';
        allResults = [];

        // 过滤掉 GT文件和manifest
        var validRecs = currentRecords.filter(function(r) {
            var fn = (r.filename || '').toLowerCase();
            return fn.indexOf('_gt') < 0 && fn.indexOf('manifest') < 0;
        });
        if (validRecs.length === 0) {
            statusEl.textContent = '无有效数据文件。';
            statusEl.style.color = '#dc2626';
            return;
        }

        // 分批处理，保持UI响应
        var k = 0, total = validRecs.length, errors = 0;
        function nextChunk() {
            var end = Math.min(k + 300, total);
            for (; k < end; k++) {
                var rec = validRecs[k];
                try {
                    if (!rec.values || rec.values.length < 5) { errors++; continue; }
                    var r = ECGEngine.processECG(rec.values, rec.sampleRate || 360);
                    r.filename = rec.filename; r.truthLabel = rec.label;
                    allResults.push(r);
                } catch(e) { errors++; }
            }
            statusEl.textContent = k + '/' + total + (errors ? ' (跳过' + errors + '个错误)' : '');
            if (k < total) setTimeout(nextChunk, 0);
            else {
                statusEl.textContent = '完成! ' + allResults.length + ' 条';
                statusEl.style.color = '#16a34a';
                currentRecordIndex = 0;
                currentResult = allResults[0];
                updateDashboard(); updateChart(); updateTable();
                updateDiagnosis(); updateReport(); updateRecordNav();
                updateBatchSummary();
                saveResults();
                navigateTo('overview');
            }
        }
        nextChunk();
    }

    // --- Batch Summary ---
    function updateBatchSummary() {
        var batchDiv = document.getElementById('batchSummary');
        if (allResults.length < 2) { batchDiv.style.display = 'none'; return; }
        batchDiv.style.display = 'block';

        var totalPeaks = 0, totalHR = 0, totalCV = 0;
        var correctCount = 0, totalWithLabel = 0, abnormalCount = 0;

        allResults.forEach(function(r) {
            totalPeaks += r.numPeaks; totalHR += r.stats.hrMean; totalCV += r.stats.rrCV;
            if (r.truthLabel) {
                totalWithLabel++;
                var pred = ECGEngine.anomalyName(r.inference ? r.inference.type : 0);
                if (r.inference && (
                    (r.truthLabel === 'normal' && r.inference.type === 0) ||
                    (r.truthLabel !== 'normal' && r.inference.type !== 0)
                )) correctCount++;
            }
            if (r.inference && r.inference.type !== 0) abnormalCount++;
        });

        var avgHR = totalHR / allResults.length;
        var avgCV = totalCV / allResults.length;
        var classAcc = totalWithLabel > 0 ? (correctCount / totalWithLabel * 100) : 0;

        document.getElementById('batchStats').innerHTML =
            '<div style="display:flex;gap:24px;flex-wrap:wrap">'
            + '<div><span class="text-muted">总记录数</span><br><b>' + allResults.length + '</b></div>'
            + '<div><span class="text-muted">总R波数</span><br><b>' + totalPeaks.toLocaleString() + '</b></div>'
            + '<div><span class="text-muted">平均心率</span><br><b>' + avgHR.toFixed(1) + ' bpm</b></div>'
            + '<div><span class="text-muted">平均CV</span><br><b>' + (avgCV * 100).toFixed(1) + '%</b></div>'
            + '<div><span class="text-muted">异常检出</span><br><b style="color:#dc2626">' + abnormalCount + '</b></div>'
            + (totalWithLabel > 0
                ? '<div><span class="text-muted">分类准确率</span><br><b style="color:#2563eb">' + classAcc.toFixed(1) + '%</b></div>'
                : '')
            + '</div>';

        var tbody = document.querySelector('#tblBatch tbody');
        var html = '';
        allResults.forEach(function(r) {
            var predLabel = ECGEngine.anomalyName(r.inference ? r.inference.type : 0);
            var isCorrect = r.truthLabel && (
                (r.truthLabel === 'normal' && (r.inference && r.inference.type === 0)) ||
                (r.truthLabel !== 'normal' && (r.inference && r.inference.type !== 0))
            );
            html += '<tr>'
                + '<td>' + r.filename + '</td>'
                + '<td>' + r.rawData.length + '</td>'
                + '<td>' + r.numPeaks + '</td>'
                + '<td>' + r.stats.hrMean.toFixed(1) + '</td>'
                + '<td>' + (r.stats.rrCV * 100).toFixed(1) + '%</td>'
                + '<td>' + (r.truthLabel || '--') + '</td>'
                + '<td style="color:' + (r.inference && r.inference.type !== 0 ? '#dc2626' : '#16a34a') + '">' + predLabel + '</td>'
                + '<td>' + (r.truthLabel ? (isCorrect ? '<span style="color:#16a34a">✓</span>' : '<span style="color:#dc2626">✗</span>') : '--') + '</td>'
                + '</tr>';
        });
        tbody.innerHTML = html;
    }

    // --- Training ---
    function initTraining() {
        document.getElementById('btnRunTraining').addEventListener('click', function() {
            if (isTraining) return;
            isTraining = true;
            var statusEl = document.getElementById('trainStatus');
            var resultsDiv = document.getElementById('trainResults');
            var btn = document.getElementById('btnRunTraining');
            btn.disabled = true;
            resultsDiv.style.display = 'none';
            statusEl.textContent = '扫描数据中...';
            statusEl.style.color = 'var(--text-secondary)';

            // 如果还没批量分析，先跑批量处理
            if (allResults.length === 0 && currentRecords.length > 0) {
                // 分批处理，保持UI响应
                var k = 0;
                function nextChunk() {
                    var end = Math.min(k + 300, currentRecords.length);
                    for (; k < end; k++) {
                        var rec = currentRecords[k];
                        var fn = rec.filename.toLowerCase();
                        if (fn.indexOf('_gt') >= 0 || fn.indexOf('manifest') >= 0 || rec.values.length < 5) continue;
                        try { var r = ECGEngine.processECG(rec.values, rec.sampleRate || 360); }
                        catch(e) { continue; }
                        r.filename = rec.filename; r.truthLabel = rec.label || '';
                        allResults.push(r);
                    }
                    statusEl.textContent = k + '/' + currentRecords.length;
                    if (k < currentRecords.length) setTimeout(nextChunk, 0);
                    else finishTrain();
                }
                nextChunk();
            } else if (allResults.length > 0) {
                finishTrain();
            } else {
                statusEl.textContent = '无数据。请先到心电记录页加载CSV文件。';
                statusEl.style.color = '#dc2626';
                isTraining = false;
                btn.disabled = false;
            }

            function finishTrain() {
                // 统计所有处理成功的记录
                var all = allResults.filter(function(r) { return r.numPeaks >= 1; });
                var n = all.length;
                if (n === 0) {
                    statusEl.textContent = '无法统计：' + allResults.length + '条记录均无R波检测。请检查数据。';
                    statusEl.style.color = '#dc2626';
                    isTraining = false; btn.disabled = false;
                    return;
                }
                var hSum = 0, hSum2 = 0, cSum = 0;
                all.forEach(function(r) { hSum += r.stats.hrMean; hSum2 += r.stats.hrMean * r.stats.hrMean; cSum += r.stats.rrCV; });
                var hm = hSum / n, hs = Math.sqrt(Math.max(0, hSum2 / n - hm * hm)), cm = cSum / n;
                trainedParams = { hrMean: hm, hrStd: hs, rrCvMean: cm, normalCount: n };
                resultsDiv.style.display = 'block';
                document.getElementById('trNormalCount').textContent = n;
                document.getElementById('trHRMean').textContent = hm.toFixed(1) + ' bpm';
                document.getElementById('trHRStd').textContent = '\u00B1' + hs.toFixed(1);
                document.getElementById('trRRCVMean').textContent = cm.toFixed(4);
                isTraining = false; btn.disabled = false;
                saveResults();
            }
        });
    }

    // --- Demo ---
    function loadDemoData() {
        if (currentResult) return;
        try {
            var demoData = generateBetterDemoSignal();
            var result = ECGEngine.processECG(demoData, 360);
            result.filename = 'demo（合成信号）';
            allResults = [result];
            currentRecordIndex = 0;
            currentResult = result;
            updateDashboard(); updateChart(); updateTable();
            updateDiagnosis(); updateReport(); updateRecordNav();
        } catch(e) {
            console.error('Demo加载失败:', e);
        }
    }

    var _demoCache = null;
    function generateBetterDemoSignal() {
        if (_demoCache) return _demoCache;
        var sr = 360, n = sr * 10, signal = [], hr = 72;
        var rrSamples = Math.round(60000.0 / hr * sr / 1000); // ~300 samples

        for (var i = 0; i < n; i++) {
            var t = i / sr;
            var val = 0;
            var beatIdx = i % rrSamples;

            // Sharp QRS complex with R peak at ~50ms into the beat
            if (beatIdx < Math.round(0.12 * sr)) {
                var tau = beatIdx / sr;
                // High-amplitude R wave
                val += 1.2 * Math.exp(-((tau - 0.05) / 0.012) * ((tau - 0.05) / 0.012));
                // Q wave
                val -= 0.15 * Math.exp(-((tau - 0.02) / 0.008) * ((tau - 0.02) / 0.008));
                // S wave
                val -= 0.25 * Math.exp(-((tau - 0.08) / 0.01) * ((tau - 0.08) / 0.01));
            }
            // T wave (after QRS)
            if (beatIdx >= Math.round(0.15 * sr) && beatIdx < Math.round(0.40 * sr)) {
                var t2 = (beatIdx - Math.round(0.15 * sr)) / sr;
                val += 0.30 * Math.exp(-((t2 - 0.12) / 0.05) * ((t2 - 0.12) / 0.05));
            }
            // P wave (before QRS at end of cycle)
            if (beatIdx >= Math.round(0.65 * sr) && beatIdx < Math.round(0.80 * sr)) {
                var t3 = (beatIdx - Math.round(0.70 * sr)) / sr;
                val += 0.10 * Math.exp(-((t3) / 0.03) * ((t3) / 0.03));
            }

            // Baseline wander
            val += 0.04 * Math.sin(2 * Math.PI * 0.15 * t);
            // Minor noise
            val += (Math.random() - 0.5) * 0.015;

            signal.push(val);
        }
        _demoCache = signal;
        return signal;
    }

    // --- Chart ---
    function initChart() {
        if (typeof Chart === 'undefined') return;
        var ctx = document.getElementById('ecgChart').getContext('2d');
        ecgChart = new Chart(ctx, {
            type: 'line',
            data: {
                datasets: [{
                    label: 'ECG信号',
                    data: [],
                    borderColor: '#3b82f6',
                    borderWidth: 1.2,
                    pointRadius: 0,
                    tension: 0.1,
                    fill: false
                }, {
                    label: 'R波检测',
                    data: [],
                    backgroundColor: '#ef4444',
                    pointRadius: 5,
                    pointStyle: 'triangle',
                    rotation: 0,
                    showLine: false,
                    pointBorderWidth: 1,
                    pointBorderColor: '#b91c1c'
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                animation: false,
                interaction: { mode: 'nearest', intersect: false },
                plugins: {
                    legend: {
                        display: true,
                        position: 'top',
                        labels: { usePointStyle: true, boxWidth: 8, boxHeight: 8, padding: 12, font: { size: 12 } }
                    },
                    tooltip: { enabled: true, mode: 'index', intersect: false }
                },
                scales: {
                    x: {
                        type: 'linear',
                        title: { display: true, text: '采样点', font: { size: 11 } },
                        ticks: { font: { size: 10 }, maxTicksLimit: 15 },
                        grid: { color: '#f1f5f9' }
                    },
                    y: {
                        title: { display: true, text: '幅值 (mV)', font: { size: 11 } },
                        ticks: { font: { size: 10 } },
                        grid: { color: '#f1f5f9' }
                    }
                }
            }
        });

        document.getElementById('chkShowPeaks').addEventListener('change', updateChart);
        document.getElementById('selTimeRange').addEventListener('change', updateChart);
    }

    function updateChart() {
        if (!currentResult || !currentResult.rawData || !ecgChart || typeof Chart === 'undefined') return;

        var showPeaks = document.getElementById('chkShowPeaks').checked;
        var range = document.getElementById('selTimeRange').value;

        var raw = currentResult.rawData;
        var peaks = currentResult.peaks;
        var totalLen = raw.length;
        if (totalLen === 0) return;
        var n = (range !== 'all' && totalLen > parseInt(range)) ? parseInt(range) : totalLen;

        var step = Math.max(1, Math.ceil(n / 5000));
        var ecgPoints = [];
        for (var i = 0; i < n; i += step) {
            var val = raw[i];
            if (val != null && !isNaN(val)) ecgPoints.push({ x: i, y: val });
        }

        var peakPoints = [];
        if (showPeaks) for (var i = 0; i < peaks.length; i++)
            if (peaks[i] < n) {
                var pv = raw[peaks[i]];
                if (pv != null && !isNaN(pv)) peakPoints.push({ x: peaks[i], y: pv });
            }

        ecgChart.data.datasets[0].data = ecgPoints;
        ecgChart.data.datasets[1].data = peakPoints;
        ecgChart.update('none');
    }

    // --- Dashboard ---
    function updateDashboard() {
        if (!currentResult) return;
        var s = currentResult.stats;
        document.getElementById('metricPeaks').textContent = currentResult.numPeaks.toLocaleString();
        document.getElementById('metricHR').innerHTML = s.hrMean.toFixed(0) + ' bpm ' + getHRBadge(s.hrMean);
        document.getElementById('metricHRStd').textContent = s.hrStd.toFixed(1) + ' bpm';
        document.getElementById('metricRR').textContent = s.rrMean.toFixed(0) + ' ms';
        document.getElementById('metricCV').textContent = (s.rrCV * 100).toFixed(1) + '%';
    }

    function getHRBadge(hr) {
        if (hr >= 60 && hr <= 100)
            return '<span class="badge badge-green">正常</span>';
        if (hr > 100)
            return '<span class="badge badge-orange">偏快</span>';
        return '<span class="badge badge-purple">偏慢</span>';
    }

    // --- Table ---
    function updateTable() {
        if (!currentResult) return;
        document.getElementById('peakCount').textContent = '共 ' + currentResult.numPeaks + ' 个';
        var tbody = document.querySelector('#tblPeaks tbody');
        var html = '';
        var maxShow = Math.min(currentResult.numPeaks, 100);
        var sr = currentResult.sampleRate;
        for (var i = 0; i < maxShow; i++) {
            var rr = '--', hr = '--';
            if (i > 0) {
                var diffMs = (currentResult.peaks[i] - currentResult.peaks[i-1]) * 1000.0 / sr;
                rr = diffMs.toFixed(1);
                hr = (60000.0 / diffMs).toFixed(1);
            }
            html += '<tr class="peak-row" data-idx="' + i + '" style="cursor:pointer" title="点击删除此R波">'
                + '<td>' + (i + 1) + '</td>'
                + '<td>' + currentResult.peaks[i] + '</td>'
                + '<td>' + currentResult.peakAmps[i].toFixed(4) + '</td>'
                + '<td>' + rr + '</td>'
                + '<td>' + hr + '</td>'
                + '</tr>';
        }
        tbody.innerHTML = html;
        // 点击行删除R波
        tbody.querySelectorAll('.peak-row').forEach(function(row) {
            row.addEventListener('click', function() {
                var idx = parseInt(this.getAttribute('data-idx'));
                if (confirm('删除第 ' + (idx+1) + ' 个R波?')) {
                    _undoStack.push({ peaks: currentResult.peaks.slice(), amps: currentResult.peakAmps.slice() });
                    currentResult.peaks.splice(idx, 1);
                    currentResult.peakAmps.splice(idx, 1);
                    currentResult.numPeaks = currentResult.peaks.length;
                    if (currentResult.peaks.length >= 2) {
                        var rrMs = [];
                        for (var pi = 1; pi < currentResult.peaks.length; pi++) {
                            var d = currentResult.peaks[pi] - currentResult.peaks[pi-1];
                            if (d > 0) rrMs.push(d * 1000.0 / currentResult.sampleRate);
                        }
                        currentResult.rrMs = rrMs; currentResult.numRR = rrMs.length;
                        var sum = 0, sum2 = 0;
                        rrMs.forEach(function(v) { sum += v; sum2 += v*v; });
                        var rm = sum / rrMs.length, vr = sum2 / rrMs.length - rm*rm;
                        if (vr < 0) vr = 0;
                        currentResult.stats.rrMean = rm; currentResult.stats.rrCV = Math.sqrt(Math.max(0,vr))/(rm||1);
                        currentResult.stats.hrMean = rm > 0 ? 60000 / rm : 0;
                    }
                    updateTable(); updateChart(); updateDashboard();
                    document.getElementById('btnUndoPeak').style.display = 'inline-block';
                }
            });
        });
    }

    // --- Diagnosis ---
    function updateDiagnosis() {
        if (!currentResult) return;

        var inf = currentResult.inference;
        var anomalyName = ECGEngine.anomalyName(inf.type);
        var resultText, conclusion;

        if (inf.type === 0) {
            resultText = '窦性心律';
            conclusion = '正常心电图';
        } else if (inf.type === 1) {
            resultText = '窦性心动过速';
            conclusion = '心率偏快，请咨询医生';
        } else if (inf.type === 2) {
            resultText = '窦性心动过缓';
            conclusion = '心率偏慢，请咨询医生';
        } else if (inf.type === 3) {
            resultText = '心律失常';
            conclusion = 'RR间期变异较大，建议进一步检查';
        } else if (inf.type === 4) {
            resultText = '疑似停搏';
            conclusion = '检测到长RR间期，请立即就医';
        } else {
            resultText = '混合型异常';
            conclusion = '存在多种异常模式，请咨询医生';
        }

        document.getElementById('diagResult').textContent = resultText;
        document.getElementById('diagType').textContent = anomalyName;
        document.getElementById('diagGlobal').textContent = anomalyName;

        // Score bars
        var labelNames = ['正常', '心动过速', '心动过缓', '心律不齐', '停搏', '混合异常'];
        var maxScore = Math.max.apply(null, inf.scores);
        if (maxScore < 0.01) maxScore = 1;

        var barsHtml = '';
        labelNames.forEach(function(name, i) {
            var pct = Math.round(inf.scores[i] / maxScore * 100);
            var color = inf.scores[i] === maxScore ? '#2563eb' : '#cbd5e1';
            var fontWeight = inf.scores[i] === maxScore ? '600' : '400';
            barsHtml += '<div class="score-bar-row" style="font-weight:' + fontWeight + '">'
                + '<span class="score-bar-label">' + name + '</span>'
                + '<div class="score-bar-track"><div class="score-bar-fill" style="width:' + pct + '%;background:' + color + '"></div></div>'
                + '<span style="font-size:10px;color:var(--text-secondary);min-width:30px">' + inf.scores[i].toFixed(1) + '</span>'
                + '</div>';
        });
        document.getElementById('scoreBars').innerHTML = barsHtml;

        var conclusionEl = document.getElementById('diagConclusion');
        conclusionEl.innerHTML = '<span>结论: ' + conclusion + '</span>';
        if (inf.type === 0) {
            conclusionEl.style.background = '#f0fdf4';
            conclusionEl.style.color = '#16a34a';
        } else {
            conclusionEl.style.background = '#fef2f2';
            conclusionEl.style.color = '#dc2626';
        }
    }

    function updateReport() {
        if (!currentResult) return;
        document.getElementById('reportContent').textContent = ECGEngine.generateReport(currentResult);
    }

    // --- Export ---
    function initExport() {
        document.getElementById('btnExportReport').addEventListener('click', function() {
            if (!currentResult) {
                alert('请先加载心电数据。');
                return;
            }
            var report = ECGEngine.generateReport(currentResult);
            var blob = new Blob(['\uFEFF' + report], { type: 'text/plain;charset=utf-8' });
            var url = URL.createObjectURL(blob);
            var a = document.createElement('a');
            a.href = url;
            a.download = 'ECG_Report_' + (currentResult.filename || 'analysis') + '.txt';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
        });
    }

    function initUndoPeak() {
        document.getElementById('btnUndoPeak').addEventListener('click', function() {
            if (_undoStack.length === 0) return;
            var prev = _undoStack.pop();
            currentResult.peaks = prev.peaks;
            currentResult.peakAmps = prev.amps;
            currentResult.numPeaks = prev.peaks.length;
            updateTable(); updateChart(); updateDashboard();
            if (_undoStack.length === 0) this.style.display = 'none';
        });
    }

    // --- Settings ---
    function initSettings() {
        var settings = ['setSPKIAlpha', 'setNPKIAlpha', 'setThreshInit', 'setSampleRate', 'setFilterWindow', 'setTolerance'];
        settings.forEach(function(id) {
            document.getElementById(id).addEventListener('change', function() {
                applySettings();
            });
        });
    }

    function applySettings() {
        var config = {
            spkiAlpha: parseFloat(document.getElementById('setSPKIAlpha').value) || 0.125,
            npkiAlpha: parseFloat(document.getElementById('setNPKIAlpha').value) || 0.125,
            thresholdInit: parseFloat(document.getElementById('setThreshInit').value) || 0.5,
            sampleRate: parseInt(document.getElementById('setSampleRate').value, 10) || 360,
            filterWindow: parseInt(document.getElementById('setFilterWindow').value, 10) || 5,
            toleranceMs: parseInt(document.getElementById('setTolerance').value, 10) || 150
        };
        ECGEngine.updateConfig(config);

        if (currentResult && currentResult.rawData) {
            currentResult = ECGEngine.processECG(currentResult.rawData, config.sampleRate || currentResult.sampleRate);
            currentResult.filename = allResults[currentRecordIndex] ? allResults[currentRecordIndex].filename : 'reprocessed';
            currentResult.truthLabel = allResults[currentRecordIndex] ? allResults[currentRecordIndex].truthLabel : '';
            allResults[currentRecordIndex] = currentResult;
            updateDashboard();
            updateChart();
            updateTable();
            updateDiagnosis();
            updateReport();
        }
    }

    function navigateTo(page) {
        document.querySelectorAll('.nav-item').forEach(function(n) { n.classList.remove('active'); });
        var target = document.querySelector('.nav-item[data-page="' + page + '"]');
        if (target) target.classList.add('active');
        document.querySelectorAll('.page').forEach(function(p) { p.classList.remove('active'); });
        document.getElementById('page-' + page).classList.add('active');
        if (page === 'report') { updateReport(); updateBatchSummary(); }
        if (page === 'settings') { showSavedCount(); }
    }

    function initDownloadBtn() {
        document.getElementById('btnDownloadAll').addEventListener('click', downloadAllResults);
        showSavedCount();
    }

    // --- Auto-save to localStorage ---
    function saveResults() {
        saveResultsDB();
        if (allResults.length === 0) return;
        try {
            var s = []; allResults.forEach(function(r) {
                var inf = r.inference||{type:0,scores:[0,0,0,0,0,0]};
                s.push({file:r.filename,hr:r.stats.hrMean.toFixed(1),type:ECGEngine.anomalyName(inf.type)});
            });
            localStorage.setItem(STORAGE_KEY, JSON.stringify(s));
        } catch(e) {}
    }

    function loadResults() {
        try { var r = localStorage.getItem(STORAGE_KEY); return r ? JSON.parse(r) : []; } catch(e) { return []; }
    }

    function showSavedCount() {
        loadResultsDB(function(dbResults) {
            var count = dbResults.length || loadResults().length;
            var el = document.getElementById('trainStatus');
            if (count > 0) { el.textContent = '已保存 ' + count + ' 条 (DB)'; el.style.color = '#16a34a'; }
        });
    }

    function downloadAllResults() {
        loadResultsDB(function(saved) {
            if (saved.length === 0) { alert('暂无保存的结果。'); return; }
        var csv = '\uFEFF文件,时间,样本数,R波数,心率(bpm),HR标准差,RR均值(ms),CV(%),标签,AI诊断\n';
        saved.forEach(function(r) {
            csv += [r.file, r.time, r.samples, r.peaks, r.hr, r.hrStd, r.rrMean, r.cv, r.label, r.type].join(',') + '\n';
        });
        var blob = new Blob([csv], { type: 'text/csv;charset=utf-8' });
        var a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = 'ECG_Results_' + new Date().toISOString().slice(0,10) + '.csv';
        document.body.appendChild(a); a.click(); document.body.removeChild(a);
        URL.revokeObjectURL(a.href);
        });
    }
})();
