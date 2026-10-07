const { chromium } = require('playwright');
const { spawn, spawnSync } = require('child_process');
const fs=require('fs');
(async()=>{
 const path=require('path');
 const root=process.env.SOFTPLC_TEST_ROOT||path.resolve(__dirname,'../..');
 const portable=process.env.SOFTPLC_TEST_PORTABLE==='1';
 const childEnv={...process.env};
 if(portable){
  childEnv.PATH=path.join(process.env.SystemRoot,'System32')+';'+process.env.SystemRoot;
  childEnv.SOFTPLC_COMPILER='mingw';
  for(const key of ['SOFTPLC_TOOLCHAIN','INCLUDE','LIB','LIBPATH','VCINSTALLDIR','VSINSTALLDIR','PYTHONPATH','PYTHONHOME'])delete childEnv[key];
  if(spawnSync(path.join(process.env.SystemRoot,'System32/where.exe'),['cl.exe'],{env:childEnv}).status===0)throw Error('MSVC unexpectedly available on test PATH');
 }
 const proc=portable
  ?spawn(process.env.ComSpec||'cmd.exe',['/d','/c','Start-Studio.cmd','--no-browser'],{cwd:root,env:childEnv,stdio:['ignore','pipe','pipe']})
  :spawn(process.platform==='win32'?'python':'python3',['tools/plc_studio.py','--no-browser'],{cwd:root,env:childEnv,stdio:['ignore','pipe','pipe']});
 let output='',errors='';proc.stderr.on('data',d=>errors+=d);
 let browser,peer;
 try{
  const url=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Server startup timeout '+errors)),10000);proc.stdout.on('data',d=>{output+=d;const m=output.match(/http:\/\/127\.0\.0\.1:\d+\/#[\w-]+/);if(m){clearTimeout(timer);resolve(m[0]);}});proc.on('exit',()=>{clearTimeout(timer);reject(Error('Server exited '+errors));});proc.on('error',e=>{clearTimeout(timer);reject(e);});});
  browser=await chromium.launch({headless:true,channel:process.platform==='win32'?'msedge':undefined,args:['--no-sandbox']});
  const page=await browser.newPage({viewport:{width:1440,height:1020}});const issues=[];page.on('pageerror',e=>issues.push(e.message));
  await page.goto(url);await page.getByRole('heading',{name:'OB1',exact:true}).waitFor();
  await page.locator('#build').click();await page.waitForFunction(()=>!document.getElementById('load').disabled||document.getElementById('buildState').textContent==='Build failed',{},{timeout:300000});
  if(await page.locator('#load').isDisabled())throw Error(await page.locator('#buildLog').textContent());
  await page.locator('#load').click();await page.waitForFunction(()=>!document.getElementById('run').disabled);
  await page.locator('#run').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='RUN');
  await page.getByRole('button',{name:'Online watch',exact:true}).click();
  const start=page.locator('#watchRows tr').filter({has:page.getByText('DB1.Start',{exact:true})});
  await start.locator('select').selectOption('1');await start.getByRole('button',{name:'Write',exact:true}).click();
  await page.waitForFunction(()=>[...document.querySelectorAll('#watchRows tr')].some(r=>r.firstElementChild.textContent==='MotorOutput'&&r.querySelector('.value').textContent==='TRUE'));
  await page.screenshot({path:root+'/build/studio/watch.png',fullPage:true});
  await page.getByRole('button',{name:'Networks',exact:true}).click();await page.screenshot({path:root+'/build/studio/networks.png',fullPage:true});
  await page.locator('#stop').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='STOP');
  // Exercise the I/O editor and a real peer from the extracted package.
  const peerPython=portable?path.join(root,'portable/python/python.exe'):(process.platform==='win32'?'python':'python3');
  peer=spawn(peerPython,['tools/io_peer.py','--protocol','tcp','--port','15000'],{cwd:root,env:childEnv,stdio:['ignore','pipe','pipe']});
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('I/O peer startup timeout')),10000);peer.stdout.on('data',d=>{if(d.toString().includes('listening')){clearTimeout(timer);resolve();}});peer.on('error',reject);peer.on('exit',code=>{if(code){clearTimeout(timer);reject(Error('I/O peer exited '+code));}});});
  await page.getByRole('button',{name:'I/O connections',exact:true}).click();
  await page.locator('#addIo').click();
  if(await page.locator('#ioLinks select').nth(1).locator('option').count()!==5)throw Error('Missing I/O protocol option');
  await page.locator('#applyIo').click();
  await page.waitForFunction(()=>document.getElementById('ioStatus').textContent.includes('online'));
  await page.locator('#run').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='RUN');
  await page.screenshot({path:root+'/build/studio/io-connections.png',fullPage:true});
  await page.locator('#stop').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='STOP');
  await page.locator('#disconnectIo').click();
  await page.waitForFunction(()=>!document.getElementById('ioStatus').textContent.includes('online'));
  if(issues.length)throw Error(issues.join('\n'));
  if(portable){
   const status=await page.evaluate(async()=>await (await fetch('/api/status',{headers:{'X-PLC-Token':sessionStorage.getItem('plc-token')}})).json());
   if(!status.build.log.includes('w64devkit')||!status.build.log.includes('g++.exe'))throw Error('Portable build did not use bundled GCC');
   console.log('PASS: bundled Python/GCC; source build, DLL load and monitoring with only Windows directories on PATH.');
  }
  console.log('PASS: browser compile → load → RUN → online write → live output → STOP; no page errors.');
 }finally{if(peer)peer.kill();if(browser)await browser.close();if(process.platform==='win32')spawnSync(path.join(process.env.SystemRoot,'System32/taskkill.exe'),['/PID',String(proc.pid),'/T','/F']);else proc.kill('SIGINT');}
})().catch(e=>{console.error(e);process.exitCode=1;});
