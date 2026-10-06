const { chromium } = require('playwright');
const { spawn } = require('child_process');
const fs=require('fs');
(async()=>{
 const root=require('path').resolve(__dirname,'../..');
 const proc=spawn(process.platform==='win32'?'python':'python3',['tools/plc_studio.py','--no-browser'],{cwd:root,stdio:['ignore','pipe','pipe']});
 let output='',errors='';proc.stderr.on('data',d=>errors+=d);
 const url=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('Server startup timeout '+errors)),10000);proc.stdout.on('data',d=>{output+=d;const m=output.match(/http:\/\/127\.0\.0\.1:\d+\/#[\w-]+/);if(m){clearTimeout(timer);resolve(m[0]);}});proc.on('exit',()=>reject(Error('Server exited '+errors)));});
 let browser;
 try{
  browser=await chromium.launch({headless:true,channel:process.platform==='win32'?'msedge':undefined,args:['--no-sandbox']});
  const page=await browser.newPage({viewport:{width:1440,height:1020}});const issues=[];page.on('pageerror',e=>issues.push(e.message));
  await page.goto(url);await page.getByRole('heading',{name:'OB1',exact:true}).waitFor();
  await page.locator('#build').click();await page.waitForFunction(()=>!document.getElementById('load').disabled,{},{timeout:180000});
  await page.locator('#load').click();await page.waitForFunction(()=>!document.getElementById('run').disabled);
  await page.locator('#run').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='RUN');
  await page.getByRole('button',{name:'Online watch',exact:true}).click();
  const start=page.locator('#watchRows tr').filter({has:page.getByText('DB1.Start',{exact:true})});
  await start.locator('select').selectOption('1');await start.getByRole('button',{name:'Write',exact:true}).click();
  await page.waitForFunction(()=>[...document.querySelectorAll('#watchRows tr')].some(r=>r.firstElementChild.textContent==='MotorOutput'&&r.querySelector('.value').textContent==='TRUE'));
  await page.screenshot({path:root+'/build/studio/watch.png',fullPage:true});
  await page.getByRole('button',{name:'Networks',exact:true}).click();await page.screenshot({path:root+'/build/studio/networks.png',fullPage:true});
  await page.locator('#stop').click();await page.waitForFunction(()=>document.getElementById('cpuState').textContent==='STOP');
  if(issues.length)throw Error(issues.join('\n'));
  console.log('PASS: browser compile → load → RUN → online write → live output → STOP; no page errors.');
 }finally{if(browser)await browser.close();proc.kill('SIGINT');}
})().catch(e=>{console.error(e);process.exitCode=1;});
